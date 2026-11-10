#!/bin/zsh
# Link the extracted DLC trees into the VFS (/dev_hdd0/game/BLUS30721) by leaf
# symlinks, mirroring what Sony's installer writes on the console. Part of
# tools/extract_dlc.sh, split out so it can be re-run on its own.
#
#   tools/link_dlc.sh
set -e
here=${0:A:h}; source "$here/env.sh"

game="$ASURA_HDD0/game/BLUS30721"
mkdir -p "$game/USRDIR"
total=0
for cid in "$ASURA_DLC_OUT"/*(/N); do
    n=0
    for f in "$cid"/**/*(.); do
        rel=${f#$cid/}
        dest="$game/$rel"
        mkdir -p "${dest:h}"
        [ -e "$dest" ] || ln -s "$f" "$dest"
        n=$(( n + 1 ))
    done
    total=$(( total + n ))
    print "linked ${cid:t}: $n files"
done
print "total: $total files under $game"
