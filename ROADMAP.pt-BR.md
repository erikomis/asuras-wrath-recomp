# Roadmap

🇧🇷 **Português (BR)** · [English](ROADMAP.md) *(a traduzir)*

Estado do port nativo do Asura's Wrath (`BLUS30721`) para macOS/arm64 e o que
vem a seguir. Mesma organização dos roadmaps do Afro Samurai e dos outros
ports. `[x]` = feito e verificado; `[ ]` = a fazer; "(em andamento)" = sendo
feito.

## Resumo (10/11/2026)

- **Descoberta boa:** o disco já é a versão 01.02 do executável — o EBOOT do
  patch 1.02 oficial é byte-idêntico ao EBOOT do disco (SHA-256 conferido).
  As DLCs exigem 1.02, então não há um segundo executável para recompilar.
- **As DLCs já estão no port** (baixadas da CDN da Sony, descriptografadas e
  linkadas no VFS): a **Part IV: Nirvana, episódios 19-22 — o final
  verdadeiro** —, os Lost Episodes (crossover com Street Fighter) e o patch.
  Falta o jogo rodar até elas (e o `.EDAT` de configuração, abaixo).
- **Toolkit:** ps3recomp (worktree `../ps3recomp-asura`, branch
  `asura/r9-base`, a partir do r9 upstream). Correções do título serão
  controladas por variável de ambiente, como no port do Afro Samurai.

## Feito

- [x] O disco inventariado: `BLUS30721` v01.02 (SELF `key_rev 0x0016`),
      UE3 (`BCGAME` da CyberConnect2, `COOKEDPS3`, `PS3TOC.TXT`), Bink,
      multi-idioma (En,Ja,Fr,De,Es,It), 6,5 GB em `USRDIR`.
- [x] `EBOOT.BIN` descriptografado sem o RPCS3 (`tools/decrypt_self.py` +
      `tools/fetch_keys.py`: 28 chaves APP + 16 NPDRM + as 3 nomeadas, do
      key_vault.cpp público do RPCS3; gravadas fora do repositório, mode 600).
- [x] O patch 1.02 baixado e conferido: o EBOOT dele é idêntico ao do disco.
- [x] As DLCs catalogadas por content id **lido do cabeçalho de cada .pkg**
      (as listas de terceiros trocam os episódios entre si; não confiar):
      `DLCBIGEXPAND0000` (Part IV: Nirvana, 1,9 GB), `DLCCOLLABO010000`
      (Lost Ep 1 vs Ryu), `DLCCOLLABO020000` (Lost Ep 2 vs Akuma),
      `ASIAEPISODE_11_5`, `ASIAEPISODE15PS3` (interlúdios), `PATCH02000000000`.
- [x] As DLCs baixadas e desempacotadas (`fetch_dlc.sh` / `extract_dlc.sh` /
      `link_dlc.sh`): 617 arquivos da Part IV, 111 do Lost Ep 1, 129 do Lost
      Ep 2, linkados por folha em `rpcs3/dev_hdd0/game/BLUS30721/` — o layout
      do instalador da Sony, que é onde o jogo procura.
- [x] Os scripts: `rebuild.sh` (com etapa `dlc`), `fetch_dlc.sh`,
      `extract_dlc.sh`, `link_dlc.sh`, `fetch_keys.py`, e os do Afro Samurai
      adaptados (`build.sh` com proteção de memória/disco, `run.sh`, `probe.sh`,
      `mount.sh`, `lift_spu.sh`).
- [x] `augment_functions.py` com o TOC detectável por porta (Afro
      0x8FFFE0, Asura 0x019AF8F8 — lido do OPD do entry).
- [ ] EBOOT analisado e traduzido (em andamento).

A análise (`analysis/BLUS30721/EBOOT.*.json`): **30.350 funções** (2,4× o
Afro Samurai), 292 imports de firmware de **21 bibliotecas** —
`cellSpurs` (39), `cellSysutil` (35), `sysPrxForUser` (33), `cellGcmSys` (28),
`sys_net` (21), `sys_fs` (19), `sceNp` (15), `sys_io` (14), `sceNpCommerce2`
(14), `sceNpTrophy` (13), `cellResc` (11), `cellRtc` (8), `cellNetCtl` (7),
`cellAudio` (7), `cellPngDec` (7), `cellGame` (6), `cellHttp` (6),
`cellSysmodule` (3), `cellSsl` (3), `cellOvis` (2), `cellUserInfo` (1).

Nota de DLC: `cellGame` + `sceNpCommerce2` + `sceNp` são o caminho pelo qual
o jogo pergunta ao sistema quais episódios adicionais estão instalados — os
stubs HLE dessas bibliotecas terão que responder "Part IV instalada" para o
final verdadeiro aparecer no menu, além do `.EDAT` ser decifrável.

## Próximos passos

### 1. O executável

- [ ] Análise (`ppu_loader.py`) e lift (`ppu_lifter.py`) completos.
- [ ] Imports do firmware inventariados (quantas bibliotecas, quais NIDs).
- [ ] Imagens SPU embutidas extraídas e traduzidas (`lift_spu.sh`).
- [ ] Compila e linka no macOS/arm64 com Metal.

### 2. Boot

- [ ] Boot → heaps e construtores estáticos → SPURS e as threads do jogo.
- [ ] Os vídeos (Bink) — o jogo é quase um anime interativo, os `.bik` são
      centrais aqui mais que no Afro.
- [ ] Save carregado/gravado (`cellSaveData`).
- [ ] Áudio.
- [ ] Menu principal.

### 3. Gameplay

- [ ] New Game → o Episódio 1, controlável.
- [ ] O visual da CC2 (cel-shading, contornos, as cores saturadas) conferido
      com o RPCS3 como referência, cena por cena.
- [ ] Os QTEs (a espinha dorsal do jogo) responsivos.
- [ ] Jogar a história até o Episódio 18.

### 4. O final verdadeiro (Part IV: Nirvana)

- [ ] `PS3-BCGAME.INI.EDAT` decifrado: o INI que diz ao jogo quais DLCs há,
      cifrado no EDAT da Sony (licença livre). Decisão: decifrar no HLE do
      `cellFs` como o RPCS3 (transparente ao jogo), um `.EDAT` por DLC.
- [ ] Episódios 19-22 carregam e jogam (mapas `S19x`-`S22x_SF.XXX`).
- [ ] Os interlúdios da Part IV (`INTERVALDRAMA_S2xx`) e as telas "to be
      continued" (`TOBECONTINUED_S2xx`).
- [ ] O Episódio 22 (*A Life Well Lived*) até o fim — **o final verdadeiro,
      a razão deste port existir.**
- [ ] (opcional) Lost Episodes: o crossover com Ryu e Akuma.
- [ ] (opcional) Interlúdios 11.5 e 15.5 (`FETCH_EXTRA=1` para baixar).

### 5. Estabilidade e desempenho

- [ ] 30 fps estáveis (o alvo do jogo no PS3), medidos.
- [ ] Sessões longas sem travar e sem vazamento.
- [ ] Encerramento limpo.

## Depois de jogável (melhorias)

- [ ] Resolução interna maior (o jogo é 720p no PS3), SMAA/FXAA sem estragar
      o cel-shading.
- [ ] 60 fps como modo experimental (cuidado com QTEs e temporizadores).
- [ ] Controles: deadzones e remapeamento para DualSense/Xbox.
- [ ] O launcher de desktop dos ports do Resistance.
- [ ] Windows e Linux, depois que o macOS estiver jogável.

## Fora do escopo por enquanto

- Recursos online e troféus.
- Os pacotes de avatar/cosmético da PSN (só episódios importam).

## Devolvendo ao upstream

- [ ] `augment_functions.py` com TOC por porta no lifter do upstream.
- [ ] O que mais o título pedir ao runtime (como no Afro: sys_mempool, JOBLIST
      do SPURS, etc.), cada correção genérica com um teste.
