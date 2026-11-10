#!/bin/zsh
# Download the Asura's Wrath add-on packages from Sony's CDN into
# $ASURA_DLC_PKG. Nothing is committed: everyone fetches their own copies from
# their own region's links. The URLs below are the BLUS30721 (US) ones.
#
#   tools/fetch_dlc.sh            download what is missing
#   tools/fetch_dlc.sh --list     only show the catalog with sizes
#
# Content IDs (read from each package's own header, not from third-party
# lists, which disagree about which URL is which episode):
#
#   DLCBIGEXPAND0000  Episode Pack Part IV: Nirvana -- episodes 19-22, the
#                     game's true ending. 1.9 GB. THE priority.
#   DLCCOLLABO010000  Lost Episode 1: At Last, Someone Angrier Than Me (Ryu).
#   DLCCOLLABO020000  Lost Episode 2: The Strongest vs. the Angriest (Akuma).
#   ASIAEPISODE_11_5  Episode 11.5: Forging Ahead (interlude, anime style).
#   ASIAEPISODE15PS3  Episode 15.5: Defiance (interlude, anime style).
#   PATCH02000000000  Patch 1.02 (the disc here is already 01.02; kept for
#                     completeness and for comparing EBOOTs).
#
# The two interludes are the biggest after Part IV; with little disk space,
# fetch them only when needed (they are listed but not downloaded by
# default unless FETCH_EXTRA=1).
set -e
here=${0:A:h}; source "$here/env.sh"

typeset -A DLC_URL DLC_NAME
DLC_URL[DLCBIGEXPAND0000]="http://zeus.dl.playstation.net/cdn/UP0102/BLUS30721_00/qFLtqlVTKkzUPyfnIMIUpuFTrbPNSyCKtxzYmiSnNPRDvFqjkNweVJzdEgVvLUdjnbqeOSrZpEfpmCRFTBnAmigiSajDNgIZlyOeT.pkg"
DLC_URL[DLCCOLLABO010000]="http://zeus.dl.playstation.net/cdn/UP0102/BLUS30721_00/jDUlbPutMgbiEzYrRdgsblApzzPRaROajXNSbgVMxcpYlkrDptOEfjGkacnLGjprMOtwwgCimghtELvmkhyFjQDQCSKtoCAjJsuym.pkg"
DLC_URL[DLCCOLLABO020000]="http://zeus.dl.playstation.net/cdn/UP0102/BLUS30721_00/WdRbiuLOPnsyIVeJCcBqzlXWToBJHpreIGCIpdItlmjNIfpqJtNLyZjnrJRBvCoarTewhtKfyMyHONgMTOpgAEzhEflYeZKgitMLz.pkg"
DLC_URL[ASIAEPISODE_11_5]="http://zeus.dl.playstation.net/cdn/UP0102/BLUS30721_00/TcmytECnQBDrSCJDzTHNGQnxPkeQKbAkDMtCkrbUHgrgYLqzwIkmAWEbtkIRBEHGIMofVZWbkEGEnAdUWacIoJWCPHDOEFHNwdWBV.pkg"
DLC_URL[ASIAEPISODE15PS3]="http://zeus.dl.playstation.net/cdn/UP0102/BLUS30721_00/dkPSpBSNMbdqQPnrJiDmIKbFeQwEtuEyNaLnUkFlnYHORNimoeIrmmHIcvGNkPHcsWegNfNiFLeJWYMmTcpKbSlCipzmGsHqqzgaZ.pkg"
DLC_URL[PATCH02000000000]="http://b0.ww.np.dl.playstation.net/tppkg/np/BLUS30721/BLUS30721_T3/9472a41e0f952e95/UP0102-BLUS30721_00-PATCH02000000000-A0102-V0100-PE.pkg"
DLC_NAME[DLCBIGEXPAND0000]="Part IV: Nirvana (episodes 19-22, true ending)"
DLC_NAME[DLCCOLLABO010000]="Lost Episode 1 (vs Ryu)"
DLC_NAME[DLCCOLLABO020000]="Lost Episode 2 (vs Akuma)"
DLC_NAME[ASIAEPISODE_11_5]="Episode 11.5 (interlude)"
DLC_NAME[ASIAEPISODE15PS3]="Episode 15.5 (interlude)"
DLC_NAME[PATCH02000000000]="Patch 1.02"

# What gets fetched by default: the true ending first, then the crossovers and
# the patch. The interludes only with FETCH_EXTRA=1 (see the header).
wanted=(DLCBIGEXPAND0000 DLCCOLLABO010000 DLCCOLLABO020000 PATCH02000000000)
[ -n "${FETCH_EXTRA:-}" ] && wanted+=(ASIAEPISODE_11_5 ASIAEPISODE15PS3)

mkdir -p "$ASURA_DLC_PKG"
for cid in $wanted; do
    out="$ASURA_DLC_PKG/UP0102-BLUS30721_00-$cid.pkg"
    if [ -f "$out" ]; then
        echo "have  $cid  $(DLC_NAME[$cid])"
        continue
    fi
    if [ "$1" = "--list" ]; then
        echo "miss  $cid  $(DLC_NAME[$cid])"
        continue
    fi
    echo "get   $cid  $(DLC_NAME[$cid])"
    curl -fsSL --retry 3 -o "$out.part" "$DLC_URL[$cid]" && mv "$out.part" "$out"
done
