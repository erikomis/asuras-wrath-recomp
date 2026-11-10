#!/bin/zsh
# Unpack the downloaded Asura's Wrath packages into $ASURA_DLC_OUT/<content id>
# and link them into the VFS where the game looks for add-on content:
# /dev_hdd0/game/BLUS30721/USRDIR (the layout Sony's installer writes).
#
#   tools/extract_dlc.sh             all downloaded packages
#   tools/extract_dlc.sh DLCBIGEXPAND0000   just the true-ending pack
#
# pkg_extract.py handles both keystreams (retail GPKG key and the debug
# SHA-1 one) and auto-detects which by checking the decrypted file table.
# With --only it decrypts one file; we always want everything.
#
# After a good extraction the .pkg can be deleted (KEEP_PKGS=1 keeps them);
# the URLs to re-fetch are in tools/fetch_dlc.sh.
set -e
here=${0:A:h}; source "$here/env.sh"

only=${1:-}
mkdir -p "$ASURA_DLC_OUT"
for pkg in "$ASURA_DLC_PKG"/UP0102-BLUS30721_00-*.pkg(N); do
    cid=${${pkg:t}:r}
    cid=${cid#UP0102-BLUS30721_00-}
    [ -n "$only" ] && [ "$only" != "$cid" ] && continue
    out="$ASURA_DLC_OUT/$cid"
    [ -d "$out" ] && { echo "have  $cid (extracted)"; continue; }
    echo "extract  $cid"
    "$PY" "$here/pkg_extract.py" "$pkg" "$out"
done

# The VFS the runner uses: rpcs3/dev_hdd0/game/BLUS30721 mirrors what the
# console has after installing the DLC. Link each package's tree in by leaf
# (like run.sh links the disc), so no data is copied twice.
"$here/link_dlc.sh"
