#!/bin/zsh
# Rebuild the Asura's Wrath port: decrypt, analyse, lift, spu, build.
#
#   tools/rebuild.sh [step ...]
#
# Steps, in order (default: all of them): stage decrypt analyse lift spu dlc build
#
#   stage    mounts the ISO read-only and copies PS3_GAME/USRDIR/EBOOT.BIN and
#            PARAM.SFO into game/ (skipped when they are already there)
#   decrypt  decrypts game/EBOOT.BIN (disc SELF, key_rev 0x0016, APP) into
#            game/EBOOT.elf with tools/decrypt_self.py and $ASURA_KEYS
#   analyse  toolkit tools/ppu_loader.py -> analysis/BLUS30721
#   lift     toolkit tools/ppu_lifter.py -> recompiled/BLUS30721
#   spu      extract the EBOOT's embedded SPU ELFs and lift them (tools/lift_spu.sh)
#   build    configure once if needed, then tools/build.sh (resource guards)
#
# Environment: see tools/env.sh. JOBS controls lifting; BUILD_JOBS controls build;
# CHUNKS the number of lifted translation units (default 12).
set -e
here=${0:A:h}; source "$here/env.sh"
E=$ASURA_EBOOT
if (( $# )); then steps=("$@"); else steps=(stage decrypt analyse lift spu build); fi
jobs=(); [ -n "$JOBS" ] && jobs=(-j "$JOBS")
: ${CHUNKS:=12}

say() { print -P "%B==> $*%b"; }

for s in $steps; do
    case $s in
    stage)
        say "stage"
        mkdir -p "$ASURA_GAME"
        if [ ! -f "$ASURA_EBOOT_BIN" ] || [ ! -f "$ASURA_GAME/PARAM.SFO" ]; then
            "$here/mount.sh"
            cp "$ASURA_MOUNT/PS3_GAME/USRDIR/EBOOT.BIN" "$ASURA_MOUNT/PS3_GAME/PARAM.SFO" "$ASURA_GAME/"
            chmod 644 "$ASURA_GAME/EBOOT.BIN" "$ASURA_GAME/PARAM.SFO"
        fi
        echo "  $ASURA_EBOOT_BIN: $(shasum -a 256 "$ASURA_EBOOT_BIN" | cut -d' ' -f1)" ;;
    decrypt)
        say "decrypt"
        if [ ! -f "$E" ]; then
            [ -f "$ASURA_EBOOT_BIN" ] || { echo "EBOOT.BIN not found: $ASURA_EBOOT_BIN (run the stage step)" >&2; exit 1; }
            [ -f "$ASURA_KEYS" ] || { echo "no key file: $ASURA_KEYS (../deadpool-recomp/tools/fetch_keys.py --out ...)" >&2; exit 1; }
            "$PY" "$here/decrypt_self.py" "$ASURA_EBOOT_BIN" -o "$E.partial" --keys "$ASURA_KEYS" \
                || { rm -f "$E.partial"; exit 1; }
            mv "$E.partial" "$E"
        fi
        [ "$(head -c 4 "$E")" = $'\x7fELF' ] || { echo "$E is not an ELF" >&2; exit 1; }
        echo "  $E: $(shasum -a 256 "$E" | cut -d' ' -f1)" ;;
    analyse)
        say "analyse"
        "$PY" "$PS3RECOMP_DIR/tools/ppu_loader.py" "$E" -o "$ASURA_ANALYSIS" ;;
    lift)
        say "lift"
        # Jump-table case targets as extra function entries (tools/jumptables.txt).
        "$PY" "$here/augment_functions.py" "$E" "$ASURA_ANALYSIS/EBOOT.functions.json" \
            "$here/jumptables.txt" "$ASURA_ANALYSIS/EBOOT.functions.aug.json"
        "$PY" "$PS3RECOMP_DIR/tools/ppu_lifter.py" "$E" "${jobs[@]}" --chunks "$CHUNKS" \
            --functions "$ASURA_ANALYSIS/EBOOT.functions.aug.json" \
            --hle-stubs "$ASURA_ANALYSIS/EBOOT.imports.json" \
            --output "$ASURA_RECOMP" ;;
    spu)
        say "spu"
        "$PY" "$PS3RECOMP_DIR/tools/extract_spu_images.py" "$E" -o "$ASURA_ANALYSIS/spu"
        "$here/lift_spu.sh" ;;
    dlc)
        say "dlc"
        "$here/fetch_dlc.sh"
        "$here/extract_dlc.sh" ;;
    build)
        say "build"
        if [ ! -f "$ASURA_BUILD/build.ninja" ]; then
            cmake -S "$ASURA_ROOT/port" -B "$ASURA_BUILD" -G Ninja \
                  -DPS3RECOMP_DIR="$PS3RECOMP_DIR" -DRECOMP_DIR="$ASURA_RECOMP" \
                  -DPython3_EXECUTABLE="$PY"
        fi
        "$here/build.sh" --check
        "$here/build.sh"
        echo ""
        echo "run it:"
        echo "  tools/run.sh" ;;
    *)
        echo "unknown step '$s' (stage decrypt analyse lift spu dlc build)" >&2; exit 2 ;;
    esac
done
say "done"
