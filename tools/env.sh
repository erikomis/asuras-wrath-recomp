# Shared settings for the shell scripts in tools/. Sourced, not run (zsh).
#
#   ASURA_ROOT      this repository (default: the parent of tools/)
#   PS3RECOMP_DIR   the toolkit checkout (default: ../ps3recomp-asura: upstream
#                  v0.19.0 ae92407b + the Afro Samurai port's runtime fixes
#                  (patches/ps3recomp-ae92407b-afro.patch, all env-gated, so
#                  they are safe for other titles), a worktree of
#                  ../ps3recomp-up-r9 on branch asura/v0.19; title choices
#                  stay env-gated in port/main.cpp and port/stubs.cpp, as in
#                  the Afro Samurai port)
#   ASURA_GAME      the game's files and the decrypted EBOOT (default:
#                   $ASURA_ROOT/game): EBOOT.elf, EBOOT.BIN, PARAM.SFO
#   ASURA_EBOOT     the decrypted PPU ELF the loader and lifter read
#   Disc data       the ISO stays in ~/Downloads behind $ASURA_ROOT/game_iso;
#                   run.sh mounts it read-only and links PS3_GAME into the VFS
#   DLC data        downloaded PSN packages under $ASURA_ROOT/dlc/pkg, unpacked
#                   (tools/pkg_extract.py) into $ASURA_ROOT/dlc/extracted and
#                   linked into the VFS at /dev_hdd0/game/BLUS30721; see
#                   docs/DLC.md (the true-ending pack, Part IV: Nirvana, is the
#                   priority)
#   PY              the shared venv (../deadpool-recomp/tools/.venv)
export ASURA_ROOT=${ASURA_ROOT:-${0:A:h:h}}
export PS3RECOMP_DIR=${PS3RECOMP_DIR:-$ASURA_ROOT/../ps3recomp-asura}
export ASURA_TITLE_ID=BLUS30721
export ASURA_GAME=${ASURA_GAME:-$ASURA_ROOT/game}
export ASURA_EBOOT=${ASURA_EBOOT:-$ASURA_GAME/EBOOT.elf}
export ASURA_EBOOT_BIN=${ASURA_EBOOT_BIN:-$ASURA_GAME/EBOOT.BIN}
export ASURA_VENV=${ASURA_VENV:-$ASURA_ROOT/../deadpool-recomp/tools/.venv}
export PY=${PY:-$ASURA_VENV/bin/python}
export ASURA_ANALYSIS=${ASURA_ANALYSIS:-$ASURA_ROOT/analysis/$ASURA_TITLE_ID}
export ASURA_RECOMP=${ASURA_RECOMP:-$ASURA_ROOT/recompiled/$ASURA_TITLE_ID}
export ASURA_BUILD=${ASURA_BUILD:-$ASURA_ROOT/build/$ASURA_TITLE_ID}
export ASURA_ISO_DIR=${ASURA_ISO_DIR:-$ASURA_ROOT/game_iso}
export ASURA_MOUNT=${ASURA_MOUNT:-$ASURA_ROOT/rpcs3/disc/ASURAS_WRATH}
# Disc SELF (key_rev 0x0016, APP) and the update package's NPDRM SELF
# (key_rev 0x0010): tools/fetch_keys.py writes every APP and NPDRM key from
# RPCS3's published key_vault.cpp into the port's own user key file.
if [ -z "${ASURA_KEYS:-}" ]; then
    export ASURA_KEYS="${HOME}/Library/Application Support/asuras-wrath-recomp/keys/self_keys.txt"
    [ -f "$ASURA_KEYS" ] || "$PY" "${0:A:h}/fetch_keys.py" >/dev/null || true
fi
# The VFS tree run.sh assembles: the disc's PS3_GAME by leaf symlinks, plus the
# save/cache trees the title creates under this repo, not under RPCS3's install.
export ASURA_VFS_ROOT=${ASURA_VFS_ROOT:-$ASURA_ROOT/rpcs3/dev_hdd0/disc/$ASURA_TITLE_ID}
# DLC: the game's add-on content (episodes 11.5/15.5, Part IV, Lost Episodes)
# is installed by Sony's packages under /dev_hdd0/game/BLUS30721/USRDIR; the
# recomp mirrors that layout in this repo's own hdd0 tree.
export ASURA_DLC_PKG=${ASURA_DLC_PKG:-$ASURA_ROOT/dlc/pkg}
export ASURA_DLC_OUT=${ASURA_DLC_OUT:-$ASURA_ROOT/dlc/extracted}
export ASURA_HDD0=${ASURA_HDD0:-$ASURA_ROOT/rpcs3/dev_hdd0}
