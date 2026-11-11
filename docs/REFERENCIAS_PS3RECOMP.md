# Asura's Wrath — o que aproveitar de outros projetos PS3Recomp

🇧🇷 Português (este arquivo)

Levantamento comparativo (11/11/2026) dos outros ports e toolkits PS3Recomp,
para reaproveitar soluções que já funcionam em macOS arm64 com Metal. Foi uma
**análise estática** de código e histórico: nada foi portado, e nenhuma das
recomendações abaixo foi aplicada ao Asura ainda.

Convenção usada em cada item:

- **Confirmado**: verificado lendo o código ou o histórico.
- **Hipótese**: plausível, mas falta uma medição ou um teste no Asura.
- Links de commit apontam para repositórios publicados do mesmo autor. Os hashes
  foram conferidos nos checkouts locais; os links em si não foram abertos online.
  Alterações que existem **só em working trees sem commit** (resistence, Deadpool
  r8, Demon's Souls) estão marcadas como tal e não têm link.

## Fontes

| Projeto | Repositório | Observação |
|---|---|---|
| PS3Recomp (upstream) | https://github.com/sp00nznet/ps3recomp | base de todos os toolkits |
| Toolkit do autor (inFAMOUS 2, Deadpool) | https://github.com/erikomis/ps3recomp | branches `infamous-work`, `deadpool-gameplay`, `feat/*` |
| Folklore | https://github.com/erikomis/folklore-decomp | toolkit macOS **commitado** e mais evoluído; fonte preferida |
| Deadpool | https://github.com/erikomis/deadpool-recomp | áudio, módulos SPU |
| Afro Samurai | https://github.com/erikomis/afro-samurai-recomp | base do Asura; overlays FMOD |
| Resistance / Resistance 2 | https://github.com/erikomis/resistance-recomp · https://github.com/erikomis/resistance2-recomp | pacing de áudio, limiter |
| Demon's Souls (fork) | https://github.com/erikomis/demons-souls-recomp (origem: https://github.com/timboat/demons-souls-recomp) | outro titular, MIT: **citar, não copiar** |
| metal-game-kit | https://github.com/erikomis/metal-game-kit | outra arquitetura (AVAudioEngine, CADisplayLink) |

- Todos os projetos são MIT. Qualquer reuso de código exige manter o aviso de
  copyright do titular; para Demon's Souls usamos só como referência de projeto.
- **Resistance 3 não tem implementação** para comparar (o diretório existe, mas
  está vazio).
- O toolkit do Asura (`ps3recomp-asura`, branch `asura/v0.19`) é o do Afro mais
  o patch em `patches/`. Os arquivos de renderizador e de shaders dele são
  praticamente idênticos aos do Afro.

## Resolvido nesta rodada

**Cache de shaders MSL** — Confirmado. O mecanismo existia no toolkit
(`rsx_shader_msl.cpp`), mas `tools/run.sh` exportava `RSX_MSL_CACHE`, nome que o
toolkit do Asura não lê (ele lê `PS3RECOMP_MSL_CACHE`), então `cache/msl` ficava
vazio. Corrigido em `tools/run.sh`. Teste: 33 arquivos `.msl` criados no primeiro
boot e reutilizados no segundo, sem regravar. Não houve ganho de tempo medível em
execução sem janela. O Deadpool resolveu o mesmo problema de outro jeito: renomeou
a variável no toolkit, [`f98f8b50`](https://github.com/erikomis/ps3recomp/commit/f98f8b50).

## Gráficos

### G1 — Fatores de blend `CONSTANT_COLOR/ALPHA` e `SET_BLEND_COLOR` (testado: **não é a causa**)

- **Origem:** Folklore, [`25c4701`](https://github.com/erikomis/folklore-decomp/commit/25c4701),
  "rsx engine: convolution filter and blend colour on Metal".
- **Confirmado no código do Asura:** `blend_factor_to_metal`
  (`rsx_metal_backend.m`) não trata os fatores 0x8001-0x8004 e cai em
  `MTLBlendFactorOne`. O engine decodifica esses fatores sem alteração e os
  entrega ao backend, mas **nunca lê** `SET_BLEND_COLOR` (0x031C, par FP16 em
  0x0390): o valor só é armazenado no register file. O Folklore tem as duas metades.
- **Hipótese que motivou o teste:** esse gap causaria o ciano saturado visto nos QTEs
  (cerca de 0,6% a 4,8% dos pixels com G e B exatamente em 255, R entre 39 e 47, em
  regiões contíguas, só em cenas de QTE).
- **Teste feito:** instrumentação só de log (`RSX_BLEND_LOG=1`, `libs/video/rsx_blend_log.h`
  no toolkit, sem efeito na renderização) em um probe de 300 s que chegou aos
  prompts de QTE "L" e "R", com o artefato ciano visível nos quadros. Resultado:
  **1.483.520 decodificações de estado de render, 411.918 com blend ligado e 0 com
  fator 0x8001-0x8004**. `SET_BLEND_COLOR` é escrito (14.009 vezes, valores como
  0x10, 0x800, 0x200), mas nenhum draw o consome por fator constante.
- **Conclusão:** nesta execução o jogo **não usa** fatores constantes, então portar a
  metade de blend do `25c4701` **não corrige o ciano**, e a hipótese está refutada
  para este trecho. O que o teste não cobre: outras cenas, e se o decodificador medido
  é o único caminho de blend (ele é o do engine usado pelo Metal).
- **Ainda abertas (não testadas):** HDR FP16 lido por um caminho errado e cortado no
  display; acumulação aditiva em MRT; filtro de convolução lido como nearest (o
  `25c4701` também traz essa parte; explicaria o aspecto "pixelado", não a
  saturação). Próximo passo sugerido: dump de shaders (`RSX_ENGINE_SHADER_DUMP`) das
  passadas que cobrem as manchas e comparação com o RPCS3 na mesma cena.

### G2 — Apresentação em janela ("tela cortando") (P1, a medir)

- **Confirmado:** a mutação de `drawableSize` ocorre fora da main thread
  (`rsx_metal_backend.m`, `eng_encode_and_commit`) e só importa ao redimensionar.
  `nextDrawable` nulo descarta o frame sem registrar nada, e nenhum dos projetos
  comparados tem contador para isso.
- **Hipótese:** a sobreposição de presents (mais presents que o vsync de 60 Hz)
  esgota o pool de 3 drawables. Falta medir em janela.
- **inFAMOUS 2 — `73c271e5`, `38db5ec0`, `8994e0e8`** (Noah Peterson, 07/10/2026;
  [`73c271e5`](https://github.com/erikomis/ps3recomp/commit/73c271e5): "Present each
  guest flip once; report true fps; time the Metal present", mais bind só do
  estado que difere do draw anterior e constantes de vertex program seletivas):
  **não estão integrados** em nenhum dos toolkits locais (`asura`, `afro`, `r7`,
  `r8`, `r9`, `resistence`); existem só em outros branches do repositório.
  O `port/main.cpp` do Asura tem uma variante própria do laço de present (consome
  o flag de flip pendente antes de olhar a contagem de flips, com o ramo seguinte
  limitado pelo vblank). Se o bug descrito no commit se aplica ao Asura é uma
  **hipótese**: a instrumentação do próprio commit seria a forma de medir.
- **Também no Folklore:** [`b1b5c35`](https://github.com/erikomis/folklore-decomp/commit/b1b5c35)
  cria o `NSApp` na main thread antes do guest (resolveu um SIGTRAP de boot, 1 em 3
  inícios, segundo a mensagem do commit; não sabemos se o Asura é afetado) e
  [`f628bd5`](https://github.com/erikomis/folklore-decomp/commit/f628bd5) remove o
  teto de 256 pacotes por grupo `BEGIN_END`. No Asura o teto existe
  (`ENG_MAX_BATCHES 256`, em `rsx_draw_engine.c`) e descarta o excedente sem
  registrar; que o jogo ultrapasse isso é hipótese.
- **Não existe em nenhum repositório:** coalescência de presents idênticos no
  engine, contador de `nextDrawable` nulo, e alinhamento do semáforo de frames em
  voo com a devolução do drawable pelo compositor.

### G3 — Fase E: qualidade e desempenho (P2)

Todos gerais, sem ocorrência confirmada de problema no Asura:

- Resolução interna `PS3_RES_SCALE` (Folklore e resistence/r8/demons, esses três
  sem commit; origem no trabalho do inFAMOUS 2).
- MetalFX espacial: `eng_metalfx` no r8 (sem commit) e via launcher no Folklore.
  Não existe versão temporal em nenhum repositório.
- Cache de shaders em três camadas no Folklore,
  [`d809d97`](https://github.com/erikomis/folklore-decomp/commit/d809d97):
  cache chaveado no build do tradutor, tradução assíncrona e `MTLBinaryArchive`.
  O commit relata 160 ms por tradução contra 8 ms do cache.
- Filtro de convolução RSX e alpha-to-coverage, Folklore
  [`25c4701`](https://github.com/erikomis/folklore-decomp/commit/25c4701) e
  [`f8ced14`](https://github.com/erikomis/folklore-decomp/commit/f8ced14); ZPASS real
  (só otimização).
- SMAA, FXAA, CAS e vibrance o Asura já tem (vieram do Afro).

## Áudio e runtime

### A1 — Observabilidade do caminho de áudio (P1)

- **Confirmado no Asura** (`libs/audio/cellAudio.c`): abre apenas o dispositivo
  padrão e não registra o formato obtido; ignora o retorno de `SDL_QueueAudio`; se o
  backend falha, o mixer sobe mesmo assim e o `cellAudioInit` retorna sucesso; o
  mixer dorme 2 ou 5 ms sem regulador de pacing.
- **Confirmado:** o mixer produz sinal. Em um probe de 100 s com `AUDIO_PEAK=1`, o
  pico foi 0 até cerca de 40 s e maior que 0 em 62 dos 102 grupos de ~1 s, com
  máximo de 0,93. O silêncio total **não** está confirmado.
- **Medido com a instrumentação `ASURA_AUDIO_DIAG=1`** (probe sem janela, 100 s,
  duas execuções): o SDL abriu o driver `coreaudio` com o formato pedido
  (48 kHz, 2 canais, 256 amostras), estado "playing", em um único dispositivo de
  saída; `SDL_QueueAudio` não falhou nenhuma vez em ~96 resumos de ~1 s; o SPU
  (imagem 1) escreve o PCM no ring em `0x58000000`, em blocos sequenciais de 8 KiB.
  Em cerca de 10 dos ~95 resumos a fila SDL chegou ao mínimo de um bloco depois de
  enfileirar, isto é, ficou vazia antes dele: **momentos de esvaziamento**. Isso não
  prova som audível nem som com conteúdo correto (falta ouvir ou fazer loopback), e a
  instrumentação altera o tempo (as amplitudes de pico variaram entre execuções).
- **Hipótese:** o problema está depois do mixer (rota, volume do sistema, esvaziamentos
  da fila) ou em fontes específicas (a task MultiStream com `flashMP3.pic`).
- **Modelos para observar:** seleção de dispositivo com fallback e log do formato
  obtido, dump WAV e estatísticas por canal. Origem: resistence
  (`audio_sdl_pick_device`, `audio_dump_*`, `audio_stats_tick`, todos sem commit) e
  Folklore. O dump precisa de limite de tamanho (disco quase cheio) e as
  estatísticas não tratam NaN/Inf.

### A2 — Pacing POSIX do mixer (P1, só com A/B)

Resistence (`audio_pace_*`), Demon's Souls e o Deadpool PES têm agenda exata em
`CLOCK_MONOTONIC` e controle da fila. Benefício para o Asura é hipótese. Riscos:
mais latência e CPU. Sem alteração de pacing nesta etapa.

### A3 — `flashMP3.pic` (P1, estudo)

- **Confirmado:** o Asura abre e lê o arquivo (69.695 bytes, assinatura ELF) e a
  task MultiStream executa código carregado em runtime, tratado pelo interpretador
  SPU. O log não liga o arquivo a segmentos, DMAs ou entradas.
- O Deadpool registra o decodificador como módulo levantado,
  [`70e982d`](https://github.com/erikomis/deadpool-recomp/commit/70e982d); o commit
  relata blocos silenciosos de 85% para 20-25%, **não** áudio perfeito.
- **Não reutilizável às cegas:** os endereços e o tamanho do módulo (LS 0x1AB10,
  span 0x10380) são do Deadpool; aplicados à base LS vista no Asura (0x37010)
  passariam de 0x40000, o limite do Local Store. Faltam o mapa de segmentos e as
  entradas do módulo do Asura.
- O Afro (`66997d3`, [link](https://github.com/erikomis/afro-samurai-recomp/commit/66997d3))
  levantou 18 plugins FMOD relocáveis; o runtime do Asura já tem
  `spu_overlay_register_region_reloc`.

### A4 — DMA de tamanho zero (P3)

Os nove toolkits mantêm a rejeição de `size == 0` em `mfc_do_transfer`. **Nenhum
resolve nada nesse ponto**, e a ligação entre esses DMAs e o áudio ausente não está
confirmada. Não há correção a importar.

### A5 — `CellPadCapabilityInfo` (P3 para o Asura)

- **Confirmado no toolkit:** o buffer de `cellPadGetCapabilityInfo` é declarado e
  zerado com 64 palavras; o contrato público tem 32 (128 bytes). O Folklore
  corrigiu isso em [`9c8f414`](https://github.com/erikomis/folklore-decomp/commit/9c8f414).
- **Confirmado para o Asura:** o EBOOT do Asura **não importa** essa função. O NID
  que parecia ser ela (0x578E3C98) é `cellPadSetPortSetting`; o comentário do
  header estava errado. Não há chamador a dimensionar, então não há evidência de
  overrun no Asura. É um defeito latente do toolkit para outros jogos.

### A6 — Outras correções gerais (P2, sem ocorrência confirmada no Asura)

- `maxContention` como nono argumento PPU de `_cellSpursWorkloadAttributeInitialize`:
  [`0b55733f`](https://github.com/erikomis/ps3recomp/commit/0b55733f).
- `spu_taskset_shutdown` com geração de endereço (fork de Demon's Souls, sem commit).
- Fairness e diagnóstico de semáforos e condvars (Folklore, r8).
- `PAD_FILE` com sticks (resistence): o do Asura já aceita botões e é o que o
  `tools/pad_driver.py` usa.

### Para preservar, não reaplicar

CRI (`spu_cri_image`), `WAIT_SIGNAL` e o rejoin do interpretador do Asura. O Afro
compara a imagem 22 diretamente, que no Asura é a task de PhysX. Substituir arquivos
inteiros pelos dos outros toolkits regrediria isso.

## Ordem sugerida

1. Investigar o ciano dos QTEs por outro caminho (G1 foi descartada): dump de shaders
   das passadas afetadas, HDR/MRT, filtro de convolução.
2. Áudio: telemetria SDL e dump limitado de PCM, antes de qualquer correção.
3. Medir a apresentação em janela antes de mexer em present ou `drawableSize`.
4. Fase E: resolução interna, cache em três camadas, MetalFX.
5. Estudo do `flashMP3.pic` com mapa de segmentos próprio.
