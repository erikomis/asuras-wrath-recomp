#!/bin/zsh
# Teste auditivo manual do Asura's Wrath no MacBook Pro (com janela).
#
#   tools/audio_listen_test.sh            # 1) tom de sanidade  2) jogo com janela + log
#   tools/audio_listen_test.sh dump       # igual, mais um dump PCM cru (~23 MB/min)
#   tools/audio_listen_test.sh resumo <log>   # so resume um log de uma execucao anterior
#
# O que ele NAO faz: nao altera mixer, pacing nem DMA; so liga os diagnosticos
# ASURA_AUDIO_DIAG=1 e AUDIO_PEAK=1 e guarda um log pequeno (so as linhas de audio).
# Quem confirma se "esta audivel" e voce, ouvindo.
set -u
here=${0:A:h}; root=${here:h}
tag=$(date +%Y%m%d-%H%M%S)
log="$root/cache/probe/audio_manual_$tag.log"

resumo() {                       # $1 = log
    local f=$1
    echo; echo "=== Resumo de $f"
    grep -m1 '\[asura-audio\] SDL open' "$f" || echo "(sem linha 'SDL open': o log nao tem o diagnostico)"
    grep '\[asura-audio\] SDL output device' "$f" | head -4
    echo -n "resumos de fila: "; grep -c '\[asura-audio\] SDL queue' "$f"
    echo -n "falhas de SDL_QueueAudio (maior valor 'failed='): "
    grep -o 'failed=[0-9]*' "$f" | sort -t= -k2 -n | tail -1 || true
    echo -n "segundos de audio com pico > 0: "
    grep -o 'audio-peak\] [0-9]* s: peak [0-9.]*' "$f" | awk '$5>0{n++} END{print n+0" de "NR}'
    echo -n "maior pico: "
    grep -o 'audio-peak\] [0-9]* s: peak [0-9.]*' "$f" | awk '{if($5>m)m=$5} END{print m+0}'
    echo -n "janelas com fila no minimo (2048 bytes): "
    grep '\[asura-audio\] SDL queue' "$f" | grep -c 'min=2048' || true
}

if [ "${1:-}" = "resumo" ]; then resumo "${2:?informe o log}"; exit 0; fi

echo "== Antes de comecar"
echo " - Central de Controle > Som: escolha 'Alto-falantes do MacBook Pro' (nao HDMI/Bluetooth)."
echo " - Ajustes do Sistema > Som: saida certa, mudo DESLIGADO, volume em ~25%."
echo " - Feche outros apps com som. Um controle fisico conectado tem prioridade sobre o teclado."
echo " - Teclado (sem controle): Enter=START, Z=Cross/confirmar, X=Circle, setas=direcional, Q/W=L1/R1."
echo

echo "== 1) Tom de sanidade: voce deve ouvir 1 s de 440 Hz"
if command -v ffplay >/dev/null; then
    SDL_AUDIODRIVER=coreaudio ffplay -nodisp -autoexit -volume 25 -loglevel error \
        -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1'
    printf "Ouviu o tom? [s/n] "; read ans
    [ "$ans" = "s" ] || { echo "Sem tom: o problema e do macOS/saida, nao do jogo. Verifique volume e saida e repita."; exit 1; }
else
    echo "ffplay nao encontrado; pulando o tom (instale com: brew install ffmpeg)."
fi

free_mb=$(df -m "$root" | awk 'NR==2{print $4}')
need=1024; [ "${1:-}" = "dump" ] && need=3072
if [ "$free_mb" -lt "$need" ]; then
    echo "Pouco espaco livre (${free_mb} MB; minimo ${need} MB para este modo). Libere espaco e repita."; exit 1
fi

echo; echo "== 2) Backup do save (o run.sh usa os saves do repositorio)"
sd="$root/rpcs3/dev_hdd0/home/00000001/savedata"
bk="$root/cache/save_backup_$tag"
cp -Rp "$sd" "$bk" && echo "backup em $bk"

extra=()
if [ "${1:-}" = "dump" ]; then
    pcm="$root/cache/probe/audio_manual_$tag.f32"
    extra=(AUDIO_WAV="$pcm")
    echo "dump PCM cru (f32le, 48 kHz, estereo) em $pcm: ~23 MB por minuto."
    echo "Teto de 400 MB (~17 min): ao atingi-lo o macOS encerra o jogo (ulimit -f so neste teste, nao e global)."
fi
df -h "$root" | tail -1 | awk '{print "espaco livre:",$4}'

echo; echo "== 3) O jogo vai abrir COM JANELA. Log (so linhas de audio): $log"
echo "   Va ate o titulo (START), depois NEW GAME ou o seu save. Ouca menu, abertura e primeiros QTEs."
echo "   Anote: o que tocou (musica/voz/efeito), estalos, pausas, som lento/rapido, e o minuto aproximado."
echo "   Para sair: feche a janela. Se travar, Ctrl-C AQUI (so mata esta execucao)."
unset PS3RECOMP_METAL_HEADLESS PS3RECOMP_METAL_FRAME_DUMP PAD_SCRIPT PAD_FILE PAD_AUTOPRESS \
      PAD_STICK PAD_NO_CONTROLLER PAD_NO_KEYBOARD PS3_NO_AUDIO SDL_AUDIODRIVER AUDIO_WAV
# O filtro guarda no maximo 20000 linhas, mas continua lendo tudo: se parasse de ler, o jogo
# receberia SIGPIPE e cairia.
(
  [ "${1:-}" = "dump" ] && ulimit -f $((400 * 1024 * 2))     # blocos de 512 bytes
  "$here/run.sh" ASURA_AUDIO_DIAG=1 AUDIO_PEAK=1 PS3_VERBOSE=0 "${extra[@]}" 2>&1
) | awk '/^\[asura-audio\] SDL|^\[audio-peak\]|^\[cellAudio\]|^\[cellPad\]/ { if (n++ < 20000) { print; fflush() } }' > "$log"

echo; echo "== Jogo encerrado."
resumo "$log"
echo
echo "Compare o save com o backup se quiser:  diff -r '$sd' '$bk' && echo igual"
echo "Reporte: tom ouvido (s/n), o que ouviu no jogo e quando, estalos/pausas, e este resumo."
