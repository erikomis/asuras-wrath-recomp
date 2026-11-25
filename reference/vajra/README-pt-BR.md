# Material reaproveitado do Vajra (Redgrave961) para o port macOS/arm64

> **STATUS DA APLICAÇÃO (25/11/2026):** revisão + aplicação patch a patch
> concluídas no worktree `../ps3recomp-asura` (branch `asura/v0.19`).
> Aplicados: 01, 02, 04, 05, 06, 07, 08, 09, 10, 13, 14, 15, 16, 17, 21 —
> cada um com commit próprio, verificação de compilação/testes e adaptação
> que preserva o trabalho local (ver tabela no fim).
> Pulados: 03 e 11 (já resolvidos localmente), 12 e 18 (dependem de
> infraestrutura que só existe no fork; o 18 destruiria a instrumentação
> SPURS local), 19 (deletaria o float exato NEON local; cherry-picks futuros
> sugeridos), 20 (arquitetura rsx_core ausente; portar o mecanismo final do
> fork para o Metal quando o 60 fps entrar em pauta).
>
> **Ação necessária no port:** o patch 05 mudou a assinatura do
> `ppu_stwcx32`/`ppu_stdcx64` (ctx explícito) — **re-rodar o lift do título**
> (`rebuild.sh`) antes do próximo build do jogo.

Fonte: <https://github.com/Redgrave961/Vajra> — recomp estático do *mesmo jogo*
(Asura's Wrath PS3, `BLES01227`/`BLUS30721` + update 1.02 — **o mesmo EBOOT do
nosso disco**) feito com ps3recomp, mas mirando Windows/Linux x86-64 + Vulkan.

- Licença: MIT (`LICENSE` nesta pasta). Se código for copiado, manter atribuição.
- Fork do ps3recomp usado por eles: <https://github.com/Redgrave961/ps3recomp>
  no commit `28475ea` (base inclui o upstream v0.19.0 `ae92407b`, a mesma do
  nosso worktree `../ps3recomp-asura`).
- Copiado em 25/11/2026. O que está aqui foi **curado**: só o que é portável e
  relevante (SPU, SPURS, HLE do firmware, lifter, frame pacing e conhecimento
  de endereços do jogo). Ficou de fora: Vulkan/SPIR-V, SSE2/AVX, UI Windows,
  input SDL, achievements/presence, SMAA/SSAA — coisas de plataforma x86 que
  não se aplicam ao nosso arm64/Metal.

## 1. O ganho mais imediato: o áudio (MultiStream / flashMP3)

**Nosso bloqueio (roadmap):** a task SPU MultiStream executa o codec
`flashMP3.pic` carregado em runtime pelo interpretador e rejeita transferências
de tamanho 0; os trechos em LS `0x21D20`/`0x37348` são código carregado fora do
`.text` da imagem.

**Como o Vajra resolve:** eles **não interpretam** — liftam o SPU do firmware
antes de rodar:

1. `files/firmware.cpp` + `files/lift_firmware.py`: extraem
   `sys/external/flashMP3.pic` e `sys/external/flashATRAC.pic` do
   `PS3UPDAT.PUP` (tar + AES com ERK/RIV públicas — chaves lá dentro) e liftam
   os PRX do firmware `libsre` (SPURS de verdade, base `0x30000000`) e
   `libresc` (`0x30100000`) com `lift_prx.py` + `prxgen.py`.
2. `files/lift_firmware_spu.py` (**não existe no nosso toolkit**): lifta cada
   `.pic` como ELF SPU relocável no link address e o registra **pelos bytes de
   código** via `spu_overlay_register_reloc_content(...)` com `image_id >= 100`.
   Quando o jogo carrega o codec em runtime, o runtime casa os bytes e despacha
   a função recompilada — nada de interpretador.
3. Fork commits `953d1fc8` (libsre de verdade), `18df3139` (plugins SPU do
   firmware como código traduzido) e `8a4b7a7b` (lado SPU do SPURS nativo) —
   patches 17, 16 e 18 nesta pasta.

Plano sugerido: extrair os dois `.pic` do PUP (firmware.cpp serve de receita,
inclusive as chaves), portar `lift_firmware_spu.py` ao nosso toolkit, adaptar o
registro ao nosso `spu_workload.c` e aposentar o `SPURS_INTERP_UNLIFTED=1` no
caminho do áudio.

## 2. Correções do lifter (batem com bugs que já tivemos)

| Patch | Correção | Por que importa pra gente |
|---|---|---|
| 03 (`335f23b7`) | jump tables decodificadas até o fim estrutural | Nós já fomos cortados no switch de 373 casos (`func_00CD65CC`, caso 0x121). A correção deles ataca a mesma classe no `ppu_lifter.py`. |
| 04 (`c55087da`) | fim de código inferido em imagens SPU "stripped" | Nossas imagens embutidas são stripped; evita lifar dado como código (e vice-versa). |
| 01/02 (`2cee833d`/`b73aaaf7`) | limites de funções por seção executável + `--code-end` | Fronteiras de função mais corretas no PPU e no lift de PRX do firmware. |
| 05 (`1fc22a59`) | `--wrap` para funções definidas pelo port + store-conditional com contexto | O mecanismo que eles usam pra substituir funções do jogo nativamente (ver `native_*` abaixo). |

## 3. HLE do firmware acurada (vários dos nossos itens do diário)

| Patch | O que corrige | Relação com o roadmap |
|---|---|---|
| 06 (`45c31c77`) | NetCtl, memory map, tamanho de memória de usuário e **cellOvis** | Nosso OOM do `cellOvisGetOverlayTableSize` virou override local; a versão deles é "firmware-accurate". |
| 07 (`9924744a`) | `_sys_spu_printf_*` | NID que tivemos que stubar na mão (boot3). |
| 08 (`1a58c1a8`) | `sys_spu_thread_group_disconnect_event_all_threads` + **ENOSYS para não-implementados** | "cellSpursQueueDetachLv2EventQueue sem implementação" ainda está no nosso em-aberto. |
| 09 (`fdf85f43`) | lwcond real (wake channel contado, sem polling de 1 ms) | Menos latência/CPU nas esperas — nos ajudaria nas esperas de vídeo. |
| 10 (`a5e0e21f`) | cellVideoOut: layout SDK do device-info | O "menu em janela cortando" pode passar por aqui. |
| 11 (`2cbb6841`) | tabela do sys_memory dimensionada pelo orçamento em bytes | Mesma classe do nosso limite de 1.024 entradas do alocador (que já corrigimos; confere a abordagem deles). |
| 12 (`72a2a671`) | chamadas/DMAs inválidos falham alto | Os DMAs de tamanho zero da task de áudio — em vez de rejeitar silencioso, logar o chamador. |
| 21 (`d06b6134`) | sceNpTrophy com o set real do título, layouts do firmware | Nós já corrigimos endianness/estruturas; este é o contra-padrão completo. |

## 4. Núcleo SPU (coerência e reservações)

| Patch | O que corrige |
|---|---|
| 13/14 (`4210c579`/`f87d3ca1`) | GETLLAR com fast path checado por sequência e **snapshots atômicos** — nossa task PhysX girava num GETLLAR no EA 0; coerência correta é pré-requisito. |
| 15 (`e66ffaf0`) | termina só a reservação que o escritor observou (evita invalidar reservas de outros SPUs). |
| 19 (`1d53d3a2`) | uma única trilha de ponto flutuante single-precision "aproximada exata" + checagem de overlay DMA mais barata. **Cuidado:** as partes de fast path são x86; a *semântica* é o que se aproveita. |
| 16/17/18 | SPURS: jobs e libsre como código traduzido, lado SPU nativo. São os commits grandes (o 18 tem ~2.9k linhas) — ler antes de decidir adotar, porque nosso runtime SPURS já tomou caminho próprio (`port/spurs_queue.cpp`, env-gates). |

## 5. Frame pacing / 60 fps (item futuro do roadmap)

- **Patch 20** (`5e280ec6`, pequeno): destrava o frame rate pelo caminho
  "no-vsync" do jogo dentro do rsx_core, com **QTEs independentes de frame
  rate** e sites de gameplay por tick.
- `files/frame_time.cpp` + `files/frame_sites.inc`: o mapa completo dos sítios
  de tempo do jogo — endereços de dados do EBOOT (`k_delta_seconds 0x019B8690`,
  `k_current_time 0x019FED10`, `k_gnatives 0x01A01824`, `k_gnames 0x01A0B014`,
  flags `0x019EEC88`...) e uma tabela de ~centenas de call sites classificados
  por modo (Add/Ratio/Lerp/Homing/TimerTick/Look/Aim...). Como o binário é o
  mesmo, **esses endereços valem 1:1 no nosso lift**. Nota: o arquivo inclui
  um header do renderer Vulkan deles; só o acesso a memória guest importa —
  portar para os nossos acessores.

## 6. `native_*`: reimplementação nativa dos subsistemas pesados

`files/native_physx.cpp` (65 KB), `native_octree.cpp` (52 KB),
`native_engine.cpp`, `native_collision.cpp`, `native_script_core.cpp` (218 KB),
mais os pequenos `native_fence/ring/script/frame.cpp`, `raster.h`, `guest.h`.

Eles **substituem** (via `--wrap`) partes do jogo por C++ nativo: PhysX,
culling por octree, edge geometry e o interpretador de UnrealScript da CC2.
Nosso enfoque é outro (rodar o código do jogo recompilado), então esses
arquivos valem como **documentação de comportamento** — em especial:

- `native_physx.cpp`: o que o jogo espera da task PhysX (a nossa task SPU de
  imagem 22/PhysX que girava no GETLLAR);
- `native_script_core.cpp`: a semântica do script UE3, referência pros nossos
  `port/stubs.cpp` e para validar QTEs;
- `native_octree.cpp`: culling — útil se algum mapa não renderizar.

## Como aplicar os patches

**Não aplicar às cegas.** Os patches foram gerados com `git format-patch` do
fork `Redgrave961/ps3recomp@28475ea`; nosso worktree `../ps3recomp-asura`
(upstream v0.19.0 + patch do Afro + commits próprios) **divergiu**, então o
apply vai dar conflito em parte deles. Uso recomendado:

```sh
# clonar o fork deles e ler o commit de interesse em contexto
git clone https://github.com/Redgrave961/ps3recomp.git /tmp/ps3recomp-vajra
cd /tmp/ps3recomp-vajra && git show 45c31c77   # ex.: cellOvis/NetCtl

# os arquivos deste diretório (patches/*.patch) servem para leitura/diff
# sem precisar da rede; cada nome carrega o sha de origem.
```

Regra de casa mantida: correção genérica entra no toolkit com teste; correção
de título entra env-gated em `port/main.cpp`/`port/stubs.cpp`.

## O que NÃO foi trazido (e por quê)

- Vulkan/SPIR-V, SMAA/SSAA, shadow filtering — renderer deles; nós temos Metal.
- SSE2/SSE4.1/AVX2 nos helpers SPU — só a semântica dos commits importa; nossa
  implementação é NEON/arm64.
- `setup_window.cpp`, `pc_options.cpp`, `presence.cpp`, `button_prompts.cpp`,
  `font.cpp`, `rl_achievements` — UI/plataforma/features pós-jogável; se o
  roadmap pedir (menu de opções in-game, launcher), o repo original tem.
- `game_files.cpp` (setup one-click de disco/update/DLC) — nós já temos esse
  fluxo resolvido (`fetch_dlc.sh`/`extract_dlc.sh`/`link_dlc.sh`).

## Registro da aplicação (toolkit `../ps3recomp-asura`)

| Patch | Commit no toolkit | Adaptação feita | Verificação |
|---|---|---|---|
| 01 loader bounds | `3a6acc91` | nenhuma (limpo) | test_loaded_image_memory: 26 checks |
| 02 code_lo `--code-end` | `81684a72` | nenhuma (limpo) | conformance lifter |
| 04 SPU code end | `713d3e22` | nenhuma (limpo) | merge_chunks OK |
| 06 NetCtl/352/cellOvis | `d78a6a7f` | **excluídos** vm.h (janelas de memória) e sys_memory.c (logging local superior) | runtime compila |
| 07 `_sys_spu_printf` | `01a9ffab`+`e2ca34ed` | stub local antigo removido antes | runtime compila |
| 08 syscall 252 + ENOSYS | `396c74ef` | nenhuma (limpo) | runtime compila |
| 10 cellVideoOut SDK | `fda`→`a5e0e21f`+`97ae0fda` | **mantidos** CellVideoOutState/GetState (fork os apaga junto com renderer que não temos) | runtime compila |
| 17 libsre/firmware SPU | `ec8155f5` | **preservados** `spu_cri_image()` e handoff `spu_dispatch` (2 hunks mesclados à mão) | runtime compila |
| 16 SPURS jobs/PIC | `1935f7c2` | nenhuma residual (entrou limpo após 04+17) | jobmods_base PASS |
| 14 GETLLAR atômico | `1e83050f` | seq counter reescrito sobre as **3 variantes** do lockline local | 5 testes SPU passam |
| 13 header do fast path | (incluído no `1e83050f`) | par inseparável do 14 | idem |
| 15 reservas por CAS | `88b79804` | reescrito à mão (sem as striped locks do fork); instrumentação local preservada | 5 testes + float exact |
| 09 lwcond real | `a18ede43` | LwmSlot local estendido (campos de trace preservados); teste do fork re-adaptado ao win32_compat e ao trylock recursivo local | 3 checks lwcond |
| 21 troféus reais | `2776be69` | post-image integral (superconjunto das correções locais); stub `cellGame_get_boot_path`; receita de teste ganha `-framework CoreFoundation` | 2 testes de troféu |
| 05 `--wrap` + stwcx ctx | `0c3065ae` | `resv_off()` local mantido; fold de st_span na variante poll; **exige re-lift do título** | 1533 checks conformance |

**Falhas pré-existentes na árvore (não causadas pelos patches; comparadas
antes/depois)**: `test_loader_memory_domain.py` (fixture linka
`_sys_memory_allocate`, renomeado localmente), `test_spu_context_registries.py`
(falta `g_ps3_ro_image_lo/hi` e hooks YDKJ no link standalone),
`tests/syscalls/test_sys_fs` e `test_ppu_thread_join` (targets CMake). As 6
falhas de build são idênticas na árvore antes dos patches.

