#!/bin/zsh
# Lift the SPU images embedded in the Asura's Wrath EBOOT and register them for
# the SPURS/workload dispatch (port/spu/spu_workloads.c).
#
#   tools/lift_spu.sh
#
# The images are the SPU ELFs extract_spu_images.py writes to
# analysis/BLUS30721/spu/ (rebuild.sh spu). Their file bytes are what the
# runtime fingerprints at dispatch time, so the content fingerprints match.
#
# What the Asura's Wrath EBOOT embeds is not catalogued yet (run
# tools/rebuild.sh spu and read the names in analysis/BLUS30721/spu/; UE3
# titles typically carry their own SPURS modules). This script is engine
# agnostic: it lifts every image it finds.
#
# Images whose executable segment carries a data tail get --code-end (end of
# the last real function), set in SPU_CODE_END below when one is found.
#
# Output: port/spu/<image>/spu_recomp.{c,h} + port/spu/spu_workloads.c. Never
# commit these: they are derived from game code.
set -e
here=${0:A:h}
source "$here/env.sh"

SPU="$ASURA_ROOT/port/spu"
IMG="$ASURA_ANALYSIS/spu"
KIT="$PS3RECOMP_DIR/tools"

typeset -A SPU_CODE_END
# SPU_CODE_END[spu_00NN]=0x....

# Extra lifter options per image. Empty for now; add entries as this title's
# images turn out to need them. The Afro Samurai port's FMOD mixer needed
# "--return-entries --merge-chunks" (a plugin returned into the middle of a
# mixer function and the dispatch resumed at the wrong entry); if an Asura's
# Wrath image shows a similar resume-in-the-middle bug, reach for those.
typeset -A SPU_EXTRA

mkdir -p "$SPU"
found=0
for elf in "$IMG"/spu_*.elf(N); do
    found=$(( found + 1 ))
    stem=${elf:t:r}
    [[ -n "${ONLY_SPU:-}" && "${stem%%_at_*}" != "$ONLY_SPU" ]] && continue
    ce=${SPU_CODE_END[${stem%%_at_*}]}
    cmd=("$PY" "$KIT/spu_lifter.py" "$elf" --auto-functions "$elf"
         --symbol-prefix "${stem}_" -o "$SPU/$stem"
         --source-name spu_recomp.c --header-name spu_recomp.h)
    [[ -n "$ce" ]] && cmd+=(--code-end "$ce")
    extra=${SPU_EXTRA[${stem%%_at_*}]}
    [[ -n "$extra" ]] && cmd+=(${=extra})
    echo "[lift_spu] ${stem}${ce:+ (code-end $ce)}"
    "${cmd[@]}" > "$ASURA_ROOT/cache/lift_spu_${stem}.log" 2>&1 || {
        echo "  FAILED, see cache/lift_spu_${stem}.log" >&2; exit 1; }
done

if [ "$found" -eq 0 ]; then
    echo "[lift_spu] no SPU images in $IMG (nothing embedded in the EBOOT?)"
    exit 0
fi

"$PY" "$KIT/build_spu_workloads.py" --images "$IMG" \
    --lifted "$SPU" --out "$SPU/spu_workloads.c" --constructor --skip-lift
