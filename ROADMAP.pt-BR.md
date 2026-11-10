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
  `asura/v0.19`): upstream v0.19.0 (`ae92407b`) **+ o patch de runtime do port
  do Afro Samurai** (`patches/ps3recomp-ae92407b-afro.patch` do repositório
  dele), que aplicou limpo — as correções dele (SPURS JOBLIST, ids reais do
  prepare-flip, mapeador de memória com controle, sys_mempool...) são
  genéricas e env-gated. O Asura é UE3/SPURS pesado como o Afro; a expectativa
  é que precise das mesmas coisas. Correções novas do título continuarão
  env-gated em port/main.cpp e port/stubs.cpp.

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
- [x] EBOOT analisado: **30.350 funções OPD**, entry 0x01994138 → código
      0x00E05D5C, TOC 0x019AF8F8.
- [x] **PPU traduzido** (`CHUNKS=24`): 66.996 funções emitidas, 18.690 alvos
      de chamada únicos, 24 unidades de tradução.
- [x] **SPU traduzido**: 27 imagens ELF embutidas no EBOOT extraídas e
      liftadas (`port/spu/` + `spu_workloads.c` com registro por
      construtor).
- [x] O toolkit (worktree `../ps3recomp-asura`, v0.19.0 + patch de runtime do
      Afro) **já descriptografa EDAT/SDAT em runtime** (`libs/filesystem/edat.c`,
      com o klicensee que o título passa ao `sceNpDrmIsAvailable`): o
      `PS3-BCGAME.INI.EDAT` da Part IV tem chance de abrir sem trabalho extra.
- [ ] Compila e linka (em andamento, sem erros até a etapa 169/225).

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

- [ ] `PS3-BCGAME.INI.EDAT` decifrado: **o toolkit v0.19 já faz isso em
      runtime** (`libs/filesystem/edat.c`: NPDRM EDAT/SDAT, com o klicensee
      que o título passa ao `sceNpDrmIsAvailable` — o UE3 da CC2 deve passar
      o dele ao abrir o INI). Verificar na prática quando o boot chegar à
      leitura de DLC; se falhar, é aqui.
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

## Diário do boot (10/11/2026)

Cada um dos travamentos achado e corrigido, na ordem:

1. **Tela preta total (boot1)**: o mount.sh linkou o disco do Afro Samurai
   no VFS (o fallback aceitava qualquer /Volumes com PS3_GAME); o jogo leu
   o PARAM.SFO do Afro, criou o cache fios como BLUS30264 e leu 0 bytes.
   Fix: reuse do mount casa o image-path resolvido via hdiutil info.
2. **Install 100%** (boot2): title id correto, ProgressBar 0→100, 1,2 GB de
   CHARTEXTURES/LIGHTING/TEXTURES.TFC no hdd0 virtual, ContentPermit ok.
3. **NIDs não resolvidos**: `_sys_spu_printf_initialize` (sysPrxForUser) e
   `cellHttpInitCookie` — stubs adicionados ao toolkit.
4. **User commands descartados (boot4/5/6)**: duas metades. (a) o
   rsx_send_event lia o mask de handlers de g_gcm_driver_info_ea, que
   nada jamais seta — jogos que registram via cellGcmSet*Handler tinham
   tudo filtrado; o mask agora vem do g_gcm_handler_mask. (b) a fila do
   ISR só nascia em sys_rsx_memory_allocate; o Asura inicializa via
   _cellGcmInitBody (caminho SDK) e a fila ficava id 0 — tudo descartado.
   rsx_isr_ensure_queue() no InitBody.

Estado do boot: 18 threads do jogo, save criado, troféus init, flips
~28/s, FIFO drena (get avança), PS3TOC.TXT e COALESCED_INT.BIN lidos —
mas draws=0: frames vazios do UE3 (só semaphore + GET_REPORT + rebind de
DMAs por frame, sem método de draw nenhum no histograma).

**Bloqueio atual**: as duas tasks SPU do fios (tasksets 0x103ADB80 e
0x10451100, image=2) estacionam em WAIT_SIGNAL com a LFQueue vazia — o
produtor PPU nunca empurra trabalho. Suspeita: o scheduler do fios no PPU
espera outra coisa primeiro. Ferramenta em mãos: SPURS_EF_SPU_REPLY
(bisturi do toolkit: fabrica o flag de resposta e vê pra onde o PPU vai).

### Investigação do bloqueio (fim do dia 10/11)

- O loop de frames vazios é o UE3 rodando "nada": só SET_VIEWPORT
  (0x0A20-0x0A3C), semáforos NV4097 (0x1D6C/0x1D70) e GET_REPORT por
  frame — nenhum método de draw, nenhum bind de textura/shader.
- O jogo NUNCA abriu nenhum .XXX de mapa nem .bik de vídeo. Só:
  PS3TOC.TXT, COALESCED_INT.BIN (localização, ~1,2 MB lidos), cache
  fios e ICON0 do save. Ele está preso ANTES do primeiro carregamento
  de conteúdo.
- As tasks SPU do fios (tasksets 0x103ADB80/0x10451100) dormem em
  WAIT_SIGNAL com a LFQueue vazia: ninguém empurrou request de IO.
  Os produtores seriam as threads "fios scheduler", que nascem, rodam
  um job e saem (design efêmero do fios — elas são recriadas depois,
  tids 11-13+; não é bug morrerem).
- SPURS_EF_SPU_REPLY=2 (fabricar o flag de resposta) faz o PPU REBOOTAR
  o boot inteiro (re-init de GCM/display buffers/SPURS): o flag não é
  "ciclo completo", é sinal de reset — fabricá-lo não é o caminho.
- O disco NÃO tem PS3-BCGAME.INI.EDAT (só a DLC Part IV tem); a config
  do UE3 vem de ENGINE.XXX/GLOBALPERSISTENTCOOKERDATA.UPK, ainda não
  lidos.

**Próximas pistas (na ordem):**
1. O spin de 30 us em 0x00CEE0BC lê o control register (put/get/ref)
   via cellGcmGetControlRegister antes de cada tick — confirmar se ele
   espera espaço no FIFO (put-get) ou ref; medir o que ele compara.
2. Achar quem deveria emitir o primeiro request fios (função de
   fios::ReadFile no PPU) e por que não chega: trace do caminho
   0x00CEE0BC → o que o tick decide fazer depois do spin.
3. Conferir se o cache.idx lido (512 bytes de header "fios") leva o
   jogo a considerar o cache válido ou se ele esperava mais entradas.
