#!/bin/zsh
# Mount the Asura's Wrath disc image read-only at $ASURA_MOUNT if it is not
# mounted yet. The ISO is never written to.
set -e
source "${0:A:h}/env.sh"
if [ -d "$ASURA_MOUNT/PS3_GAME" ]; then exit 0; fi
iso=("$ASURA_ISO_DIR"/*.iso(N))
[ -f "$iso[1]" ] || { echo "ISO not found under $ASURA_ISO_DIR (check the game_iso symlink)" >&2; exit 1; }
hdiutil attach -readonly -nobrowse -noverify -mountpoint "$ASURA_MOUNT" "$iso[1]" >/dev/null
[ -d "$ASURA_MOUNT/PS3_GAME" ] || { echo "mount failed: $ASURA_MOUNT" >&2; exit 1; }
