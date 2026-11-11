# Roadmap

🇧🇷 **Português (BR)** · [English](ROADMAP.md) *(a traduzir)*

Estado do port nativo do Asura's Wrath (`BLUS30721`) para macOS/arm64 e o que
vem a seguir. Mesma organização dos roadmaps do Afro Samurai e dos outros
ports. `[x]` = feito e verificado; `[ ]` = a fazer; "(em andamento)" = sendo
feito.

## Resumo (10/11/2026)

- **Atualização da continuação (fim do dia):** o job SPU do Bink ausente foi
  capturado, liftado e registrado; o **menu de título renderiza pela
  primeira vez** (texto/UI em faixas, contra o preto total de antes). O
  vídeo de fundo ainda congela — a thread "Bink IO" bloqueia no semáforo
  RADGAME que depende de pops da fila SPURS que param após o primeiro
  vídeo — e o OOM dos ~290 s é consequência do loop de retry. Detalhes na
  última seção do diário. **Ainda não há menu interativo confirmado.**
- **Atualização da continuação:** build e link confirmados no Apple M4 Pro;
  vídeos de carregamento e logotipos agora aparecem em Metal. Corrigidos
  filas SPURS, estruturas de troféus e limite de registros do alocador.
  **Ainda não há menu interativo nem gameplay confirmados.** Veja a seção
  final deste diário para a evidência e o diagnóstico atual.
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
- [x] Compila e linka; boot com Metal verificado no Apple M4 Pro.

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

### Continuação: erro real do boot e primeiro conteúdo carregado

As pistas de espera acima foram superadas. A amostragem de threads em
`cache/probe/continue03.tprof` mostrou a thread principal dentro do tratamento
de erro do UE3, não esperando um request fios. O hook opcional
`ASURA_GUEST_LOG=1` agora espelha a mensagem UTF-16BE de `Serialize` em
`port/stubs.cpp`, preservando o tratamento original.

O primeiro erro era `OOM -2143221883 2101314.000000 2052.064453`.
O stub `cellOvisGetOverlayTableSize` exigia uma inicialização inexistente e
declarava dois argumentos, enquanto o jogo passa um ELF SPU em memória e usa
o retorno como tamanho. O erro `0x80410701`, acrescido de `0x84`, virava um
pedido de alocação de mais de 2 GB. Os overrides do título devolvem uma tabela
vazia para GetOverlayTableSize/InitializeOverlayTable, como o fallback atual
do [RPCS3](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellOvis.cpp).
Isso é compatibilidade parcial, não implementação completa de overlays.
`ASURA_OVIS_COMPAT=0` permite repetir a comparação com o stub original.

Depois dessa correção, ENGINE.XXX, BCGAME.XXX, STARTUP.XXX e GUIDCACHE.XXX
abriram, e apareceram comandos DRAW_ARRAYS. As capturas ainda são vazias ou
de cor uniforme: draws isolados não comprovam imagem correta ou menu.

Um job cru de **Edge Geometry**, fingerprint `42E1FC8CEE0BE246`, 34.688 bytes,
era descartado por falta de registro. `tools/lift_spurs_jobs.py` agora traduz
os dumps `spujob_*.bin`, confere tamanho/fingerprint e gera o registro separado
`port/spu/spurs_jobs_register.c`. O código acaba em LS `0x7834`; a tabela de
strings seguinte foi excluída da análise de instruções. `continue11.log`
confirmou entrada e retorno desse job ao gerenciador.

Para capturar e gerar jobs faltantes:

```sh
tools/probe.sh novos-jobs 30 ASURA_GUEST_LOG=1 SPU_DUMP_MISS="$PWD/cache/probe"
source tools/env.sh
"$PY" tools/lift_spurs_jobs.py --toolkit "$PS3RECOMP_DIR"
tools/build.sh
```

A tarefa SPU de áudio (`spu_0000`, imagem 1, `spu/msngSPURS_MP3`) avançava
apesar de um WAIT_SIGNAL sem objeto. O runtime liberava essa espera por uma
mudança na geração global de LFQueue. O patch agora restringe essa exceção
a esperas com objeto; o teste `tools/test_spurs_plain_signal.py` passou
**11 verificações**. Os patches do toolkit estão preservados em `patches/`.
Os testes de helpers NEON inteiros e de ponto flutuante do SDK também passaram.

**Estado mais recente (`cache/probe/continue13.log`):** o boot abre
BC-FRONTEND.XXX, BUTTONS_SF.XXX, PAUSEMENU_SF.XXX, GFXTEXT_SF.XXX e GUIDCACHE
das DLCs. Ainda há DMAs de tamanho zero na tarefa de áudio e, depois, outro
OOM com tamanho inválido (`862895164` nessa execução), seguido de SIGSEGV.
Não está jogável. O próximo diagnóstico deve capturar a origem desse novo
tamanho de alocação e a escrita que o corrompe, além de conferir a handshake
do MultiStream com o PPU. As tentativas com helpers escalares e
`--return-entries --merge-chunks` na imagem de áudio não resolveram a falha
e foram revertidas. `SPURS_JOB_CTX_V1=1` também não foi adotado como padrão.

Também corrigidos: `env.sh` define Python antes de usá-lo ao buscar chaves;
`probe.sh` remove capturas anteriores do mesmo rótulo, aceita ausência de
frames e devolve o código de erro quando o processo termina sozinho.

### Continuação: filas reais, memória e primeiros vídeos visíveis

Os OOMs descritos acima tiveram três causas distintas, agora corrigidas:

- `cellSpursQueuePopBody` devolvia sucesso sem copiar o resultado da fila.
  Os workers interpretavam dados antigos da pilha como tamanhos de alocação.
  `port/spurs_queue.cpp` implementa push/pop, peek, wrap e espera, com acesso
  big endian e sinalização das tarefas SPU. O teste independente
  `port/tests/spurs_queue_test.cpp` cobre FIFO, fila cheia/vazia e handoffs
  bloqueantes.
- As contagens de troféus eram gravadas na ordem de bytes do host: 32 virava
  `0x20000000` no PPU. O patch de troféus corrige também os tamanhos das
  estruturas públicas e os campos numéricos de informações individuais.
- A tabela de alocações acabava em 1.024 entradas, antes dos 213 MB do
  orçamento de usuário. A capacidade agora cobre todas as páginas possíveis;
  a busca ordenada mantém o reaproveitamento e a proteção das imagens ELF.
  Passaram 3.446 verificações do alocador e 26 de imagens carregadas.

A liberação indevida de WAIT_SIGNAL também ocorria no fallback que acordava
qualquer tarefa por atividade de LFQueue. Ele agora preserva as esperas sem
objeto. O harness de 11 verificações inclui esse caminho.

Um loop infinito literal no tratamento fatal de FIOS permitia ao compilador
eliminar a verificação de ponteiro do trampoline. `-fno-finite-loops` no chunk
PPU 004 preserva esse caminho sem transformá-lo em chamada nativa a NULL.

**Tela verde resolvida:** Asura usa diretamente a primeira página da segunda
reserva do mmapper para os frames Bink. `PS3_MAPPER_WIDE=1`, herdado como
ponto de partida do Afro Samurai, reservava `0x50000000`, a heap do HLE.
Os mapas retornavam EBUSY e os descritores de textura permaneciam zerados.
O padrão deste título agora é `PS3_MAPPER_WIDE=0`: a segunda reserva começa
em `0x70000000`, sem colisão. `cache/probe/safe22_t010.ppm` mostra a animação
de carregamento, e as capturas seguintes mostram os logotipos dos vídeos.

`menu23`, teste de 150 segundos com input programado, chegou à abertura de
`TITLEMENU_BG00.BIK`, mas ficou com imagem preta. A amostragem tardia mostra
a thread principal esperando um evento e a thread de renderização em
`00874EF0`, aguardando progresso gráfico. **Ainda não está jogável.** O
próximo diagnóstico acompanha a publicação de fences e o consumo do FIFO.

### Continuação: o job do Bink registrado, o menu renderiza, o vídeo congela

O `dispatch MISS fp=0xD6E964B601AB14C0 size=528` do fim do dia anterior foi
capturado com `SPU_DUMP_MISS` (probe `jobs25`), liftado (14 funções, imagem 29)
e registrado em `port/spu/spurs_jobs_register.c` — o `lift_spu.sh` agora inclui
os dumps pendentes automaticamente. O job despacha e roda: 3.170 HITs no probe
`menu27` (com `PS3_VERBOSE=1`; o HIT de job SPURS é verbose-only e os logs
`bin/ENTER` saturam nas primeiras 4 ocorrências por processo, então um job
rodando pode não deixar nenhuma linha visível no log padrão).

**Primeiro conteúdo de menu na tela:** o frame congelado do título mostra
conteúdo em faixas horizontais (linhas 180–540) contra o preto total do
`menu23`. O input chega (PAD_SCRIPT entregue, 4 eventos) mas a tela não muda.

O diagnóstico do congelamento (amostra de threads do macOS em
`cache/probe/bink30.sample.txt`, capturada com `sample <pid> 5` — muito mais
rápida que attach de lldb num binário de 573 MB):

- A thread "Bink IO" (entry OPD `0x01994498` → `func_01522008`) bloqueia em
  `sys_semaphore_wait` do semáforo `RADGAME` (sem=1):
  `func_01522008 → func_015149E8+3740 → func_0152161C+804 → sys_semaphore_wait`.
- O log padrão limitava `semaphore_post` aos primeiros 40 registros e os pops
  da fila SPURS aos primeiros 16; as conclusões sobre a parada definitiva de
  posts e pops dependiam desse limite e foram corrigidas abaixo.
- `sys_fs_read` não aparece no log padrão. A afirmação de zero bytes lidos de
  `.BIK` foi refutada pelo trace de leitura abaixo.
- As event queues 6/7 têm capacidade 1 e alguns `port_send` retornam EBUSY,
  mas a fila 6 é consumida posteriormente. A fila 7 fica com um evento
  pendente; ainda não há evidência de que isso bloqueie o vídeo.
- O OOM dos ~290 s é **consequência** do congelamento: o jogo recria as
  tasks fios em loop de retry (CreateTask id 6.800+, ~10 alocações de 64 KB/s
  sem nenhum free — 2.932 até esgotar os 213 MB de memória de usuário).

O `tools/probe.sh` agora verifica o SIGKILL final: um probe do bink29
terminou alegando ter parado e deixou o jogo órfão rodando (ainda escrevendo
no mesmo log). Nenhum dado do jogo vai ao repositório (gitignore cobre
`port/spu/` e os dumps).

### Revisão do diagnóstico dos vídeos (probes `trace38` e `fs39`)

`trace38` (75 s, `ASURA_QUEUE_LOG=500`) mostra os cinco workers FIOS
continuando a devolver itens pelas filas SPURS até o limite configurado de
500 transferências. A fila de eventos 6 também volta a ser consumida. Logo,
o limite de logging anterior não pode ser usado como prova de que os workers
ou as filas pararam. `RADGAME` é destruído e recriado com o mesmo ID entre
vídeos; contar apenas posts por ID mistura instâncias diferentes.

`fs39` (40 s, `FLOW_FSDBG=1 PS3_FSTRACE=1`) confirma leituras corretas do
arquivo de vídeo: `00_AUTOSAVE.BIK` foi lido desde o cabeçalho até o offset
393216 + 103572 bytes, `40_CAPCOMLOGO.BIK` e `40_CC2LOGO.BIK` tiveram dados
de frame lidos, e `TITLEMENU_BG00.BIK` avançou da posição 0 até pelo menos
2490368 + 131072 bytes. Cada `fread` visível devolveu a quantidade solicitada.
A captura do probe mostra o logotipo SpeedTree renderizado. Em `trace38`, as
capturas dos segundos 40 e 60 são idênticas (mesmo SHA-1), embora o renderizador
tenha registrado novos frames até o número 1800. O `TITLEMENU_BG00.BIK` já foi
aberto quando a tela ainda exibe SpeedTree. O problema atual está **depois da
leitura inicial**, possivelmente na decodificação/apresentação dos frames ou na
sincronização com o menu; isso ainda precisa ser isolado.
O vídeo do menu e a resposta ao controle não foram verificados como funcionais.

`sem40` (65 s, `SEMTID=1`) registrou 319 chamadas de
`semaphore_post(sem=1)` ao longo da introdução, principalmente da thread PPU
22. Portanto, os posts não param no primeiro vídeo. Após abrir
`TITLEMENU_BG00.BIK`, houve posts das threads 1 e 22; o último registro desse
probe foi na linha 43350. As capturas de 45 e 60 s são idênticas, embora
continuem sendo produzidos flips. O próximo trace deve correlacionar o último
post do título com a saída do job de vídeo e o buffer apresentado, sem usar
o semáforo da introdução como causa presumida.

### Continuação: overlay SPU e pressão de memória (`job41`–`resume51`)

`job41` mostrou que a imagem SPU 1 salta para código carregado em tempo de
execução em LS `0x21D20` e `0x37348` durante a abertura do título. O dump de
256 KiB de LS (`ls42`) confirmou instruções nesses endereços, fora do `.text`
da imagem extraída. O filtro opcional `SPU_LS_DUMP_IMG` permite capturar uma
imagem específica. Com `SPU_INTERP_UNLIFTED=1`, o interpretador executa esses
trechos, mas o menu continua parado. O caminho que retorna do interpretador
agora despacha a função recompilada encontrada; `resume51` confirmou que essa
correção isolada também não libera o menu.

`mem45`–`chain47` identificaram a thread PPU 19 (`CellMemoryManager`) pedindo
páginas de 64 KiB através de `func_0089A0F8`; a cadeia de chamadas passa por
`func_00A7901C`. Em `mem256_50`, um limite experimental de 256 MiB foi todo
consumido e a alocação seguinte falhou exatamente como no limite padrão de
213 MiB. O aumento de limite foi removido: a falta de memória é o resultado
das tentativas repetidas de criar tasks durante a parada, não uma solução para
ela. As consultas de tamanho de memória não foram chamadas nos probes.

`resume51` abriu `TITLEMENU_BG00.BIK`, mas as capturas de 40, 60 e 80 s têm
SHA-1 idêntico. A task SPU MultiStream terminou após `ShutdownTaskset()` e
registrou a assertiva de inicialização. A gameplay **ainda não foi alcançada**.
O próximo diagnóstico deve ligar a conclusão das tasks de vídeo à produção e
apresentação do primeiro frame do Bink do menu, observando a fila de eventos,
o semáforo `RADGAME` e o destino dos buffers de vídeo no mesmo intervalo.

### Continuação (10–11/11): do título travado à gameplay do Episódio 1

**Estado:** o probe `play79` (470 s, sem janela, input por roteiro) passa pelo
título, cria/carrega o save, entra em NEW GAME, toca a abertura do Episódio 1,
abre o menu de pausa com START e chega aos primeiros QTEs (ícones L/R) sem
nenhum erro no log. Não verificado: áudio, a resposta aos QTEs, o restante do
episódio e a execução em janela depois destas correções.

Cinco causas, na ordem em que apareceram:

1. **Título congelado (sempre).** O runtime injetava um evento "job N
   concluído" por job SPURS em toda fila anexada. A fila 5 é do alocador em
   blocos do jogo (thread `CellMemoryManager`, `func_00A7901C`), que lê o dado
   como "aloque mais 32 KB para o slot N": 2.900 blocos, 213 MB, e o tratador
   de OOM do UE3 parava segurando a trava do alocador.
   `SPURS_JC_EVENT_FROM_JOB=1`.
2. **Congelamento intermitente (3 em 10 boots).** O chain "ShaderPatching"
   (`0x01A96980`) é só de acréscimo; cada `cellSpursRunJobChain` recomeçava do
   início e reexecutava todos os jobs, e o contador em `0x01A96900` que a
   thread de renderização espera (`func_008736F8`) varria 1..N a cada chamada.
   `SPURS_JG_WAIT_FOREVER=1`.
3. **SIGBUS ao carregar o Episódio 1.** O `switch` de 373 casos de
   `func_00CD65CC` era cortado em 256 pelo lifter; o caso 0x121 virava
   "unresolved indirect call". Corrigido no lifter; só `ppu_recomp_006.cpp`
   mudou.
4. **Parada logo após o mapa e o HUD.** Os casos especiais da task CRI de
   outro título usam o id de imagem 22, que aqui é a task de PhysX; ela lia
   parâmetros do EA 0 e girava num GETLLAR em `0x00400000`.
   `SPU_CRI_IMAGE=0`.
5. **Probes imprevisíveis.** O controle físico conectado mexia nos menus
   entre os botões do roteiro. `PAD_NO_CONTROLLER=1` em `tools/probe.sh`.

Os quatro primeiros são padrões do título em `port/main.cpp`; as mudanças do
toolkit estão em `patches/ps3recomp-asura-gameplay.patch`.

Em aberto: o áudio (a task MultiStream executa o codec `flashMP3.pic`
carregado em tempo de execução pelo interpretador e rejeita transferências de
tamanho 0); `cellSpursQueueDetachLv2EventQueue` sem implementação;
`GCM_USER_CMD_QUEUE=1` ficou ligado sem ter sido a causa de nada (hipótese
não confirmada); e o relato de "tela cortando" no menu em janela, que as
capturas sem janela não mostram.
