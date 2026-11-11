# Smoke test do Episódio 1 (sem janela, malha fechada)

🇧🇷 Português (este arquivo)

Leva o jogo do título até o início do Episódio 1 sem ninguém nos controles, usando
um probe sem janela (`tools/probe.sh`) e o `tools/pad_driver.py`. Serve para
reproduzir o ponto "NEW GAME → abertura → primeiros QTEs" depois de uma mudança no
port ou no toolkit.

**O que ele verifica e o que não verifica.** Verifica que o jogo chega ao Episódio 1
e mostra os prompts de QTE (ícones "L" e "R"). **Não** verifica a resposta aos QTEs,
o resto do episódio, o áudio nem a execução em janela.

## Por que um driver em malha fechada

Um `PAD_SCRIPT` com tempos fixos não é confiável com um save que já tem progresso:
depois do diálogo "Load successful", o cursor do título começa em EXTRAS ou em
EPISODE MENU conforme o tempo de boot, e o mesmo roteiro termina em lugares
diferentes (testado: o mesmo roteiro caiu em OPTIONS, em EXTRAS e em NEW GAME;
`PAD_CLOCK=flips` repetiu duas vezes e divergiu na terceira).

O `tools/pad_driver.py` olha o último frame que o probe grava
(`cache/probe/<rótulo>.ppm`), reconhece a tela e escreve o próximo botão no arquivo
de `PAD_FILE`, que o `cellPad` já lia (entrada "closed-loop"). Nada no jogo nem no
runtime foi alterado.

## Como rodar

Faça **backup do save** antes (NEW GAME passa por "Save successful"). Nas execuções
validadas o conteúdo do save ficou idêntico, mas não conte com isso:

```sh
cp -Rp rpcs3/dev_hdd0/home/00000001/savedata /caminho/do/backup/
```

Em um terminal, na raiz do repositório (substitua `smoke1` por um rótulo livre):

```sh
PADTMP=$(mktemp -d)                      # arquivo de comunicação com o jogo
: > "$PADTMP/pad.txt"
tools/pad_driver.py cache/probe/smoke1.ppm "$PADTMP/pad.txt" 300 "$PADTMP/driver.log" &
PROBE_SNAP=60 tools/probe.sh smoke1 300 PAD_FILE="$PADTMP/pad.txt"
wait
```

- `PROBE_SNAP=60` guarda um quadro por minuto (`smoke1_t060.ppm`, ...). Passe-o como
  variável de ambiente, na frente do comando (como argumento ele iria para o jogo).
- Libere espaço antes: o log do probe chega a dezenas de MB e cada quadro tem 2,6 MB.
- O probe define `PAD_NO_CONTROLLER=1`, então um controle físico plugado não interfere.

## O que esperar

O `driver.log` registra cada decisão. Uma execução saudável tem esta sequência
(tempos aproximados, medidos em execuções reais):

| Tempo | Tela | Tecla |
|---|---|---|
| ~40-45 s | PRESS START | START |
| ~+5 s | "Load successful" | CROSS |
| ~+5 s | título, cursor em EPISODE MENU | UP |
| ~+5 s | título, cursor em NEW GAME | CROSS |
| ~+5 s | "Select a difficulty level" (EASY marcado); às vezes o log registra isto como "close message" se a barra ainda não foi detectada, e o efeito é o mesmo | CROSS |
| ~+5 s | "Start the game on EASY?" (padrão "No") | UP, depois CROSS em "Yes" |
| ~75-83 s | "Save successful" | CROSS, e o driver entra em modo gameplay |
| ~145-155 s | cartão PRESS START do episódio, depois de ~70 s de carga | START, uma única vez |
| até ~240 s | Episódio 1 com os prompts "L" e "R" | nada |

O critério de aprovação é o quadro de 240 s ou 300 s (`smoke1_t240.ppm`,
`smoke1_t300.ppm`) mostrando os ícones "L" e "R" sobre a cena vermelha. Converta
para ver: `sips -s format png smoke1_t300.ppm --out smoke1_t300.png`.

Para ver o que o classificador enxerga em um quadro, sem rodar o jogo:

```sh
tools/pad_driver.py --classify cache/probe/smoke1_t140.ppm
# {'bar_y': 348.0, 'dialog': True}   -> diálogo de dificuldade, EASY marcado
```

## Evidência

Oito execuções chegaram aos prompts de QTE ("L" e "R") antes de 300 s, com versões
sucessivas do driver. As três últimas usaram **exatamente** o arquivo atual:

| Execução | Versão do driver | Resultado |
|---|---|---|
| `cl1` | primeira; injetava UP e START em pleno episódio | QTE aos 330 s |
| `cl4`, `cl5` | injetava só um START, mas decidia dificuldade/confirmação contando estágios | QTE; mesma sequência de 9 teclas |
| `smoke1` | idem; um falso positivo mandou um CROSS de dentro do QTE aos 267 s | QTE; levou à correção "mãos fora depois do START" |
| `smoke2` | idem; confundiu a confirmação "Yes/No" com a lista de dificuldade e apertou UP 8 vezes | QTE, mas só aos 300 s e com 20 teclas; levou à correção por característica (texto na linha y≈348) |
| `smoke3`, `smoke4`, `smoke5` | **atual** (SHA-256 começando em `2dc1b6a6`; inalterado entre as três) | QTE nas três; 9 teclas em cada, nenhuma depois do START do cartão, e o log do jogo mostra exatamente 9 `PAD_FILE press` por execução |

Nas execuções finais os quadros de 240 s e 300 s são diferentes entre si, ou seja,
a cena está animando. Três execuções não esgotam a variação do boot (o START do cartão do episódio
chegou entre ~147 s e ~175 s nessas execuções): rode o smoke test mais de uma vez
depois de qualquer mudança no driver.

## Limites conhecidos

- **O classificador é calibrado para quadros 1280×720** do probe sem janela, na
  posição das barras de seleção (título: y≈393, 428, 463, 498; PRESS START: y≈475;
  dificuldade: EASY y≈348) e das duas linhas claras do diálogo (y≈242 e y≈477).
  Outra resolução ou outro tema de menu exige recalibrar.
- **Cenas dominadas por vermelho enganam o detector de barra.** Por isso, depois do
  START do cartão do episódio o driver para de enviar teclas. Um driver que continue
  enviando teclas em gameplay vai apertar botões à toa (em uma versão anterior, isso
  enviou START e UP em pleno episódio).
- O save mostrado na descrição é o de um jogador que já passou do título. Com um save
  vazio, o fluxo do título é outro (sem "Load successful" e com o cursor em NEW GAME)
  e não foi testado com este driver.
- Se o jogo falhar antes do episódio, o driver não tem como saber: ele só envia
  teclas conforme o que vê. Confira o `driver.log` e o último quadro.
