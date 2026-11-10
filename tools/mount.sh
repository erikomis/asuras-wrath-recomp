#!/bin/zsh
# Mount the Asura's Wrath disc image read-only at $ASURA_MOUNT if it is not
# mounted yet. The ISO is never written to.
#
# An existing mount elsewhere (Finder, a previous run, /Volumes/PS3VOLUME) is
# reused: hdiutil refuses a second attach of the same image ("resource busy"),
# so the disc root is linked from wherever it is already mounted.
set -e
source "${0:A:h}/env.sh"
if [ -d "$ASURA_MOUNT/PS3_GAME" ]; then exit 0; fi
iso=("$ASURA_ISO_DIR"/*.iso(N))
[ -f "$iso[1]" ] || { echo "ISO not found under $ASURA_ISO_DIR (check the game_iso symlink)" >&2; exit 1; }

# Already mounted somewhere? Reuse that mount: the entry block in hdiutil
# info for this image ends with its /dev entries, whose last field is the
# mount point. Match on the image path (it may carry spaces; resolve symlinks
# first, hdiutil reports the real path). A /Volumes fallback is NOT used:
# with several ports' discs attached, it may link the wrong game's disc (it
# linked the Afro Samurai one once, and the game read Afro's PARAM.SFO and
# created its fios cache under Afro's title id).
real=$(readlink -f "$iso[1]" 2>/dev/null || echo "$iso[1]")
mnt=$(hdiutil info | awk -v img="$real" '
    /^image-path/ { sub(/^image-path[ \t]*: */, ""); same = ($0 == img) }
    same && /^\/dev\// {
        # "/dev/disk10<TAB>/Volumes/NAME" -- the mount point is the last
        # whitespace-separated field, however wide the gap.
        n = split($0, f, /[ \t]+/); if (n >= 2) { print f[n]; exit }
    }')
if [ -n "$mnt" ] && [ -d "$mnt/PS3_GAME" ]; then
    mkdir -p "$ASURA_MOUNT"
    ln -sfn "$mnt/PS3_GAME" "$ASURA_MOUNT/PS3_GAME"
    ln -sfn "$mnt/PS3_DISC.SFB" "$ASURA_MOUNT/PS3_DISC.SFB"
    exit 0
fi

hdiutil attach -readonly -nobrowse -noverify -mountpoint "$ASURA_MOUNT" "$iso[1]" >/dev/null
[ -d "$ASURA_MOUNT/PS3_GAME" ] || { echo "mount failed: $ASURA_MOUNT" >&2; exit 1; }
