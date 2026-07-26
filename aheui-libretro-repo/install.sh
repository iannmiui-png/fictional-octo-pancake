#!/bin/sh
# install.sh -- put the core where RetroArch will find it.
#
# The core and info directories are configurable, so this reads them out of
# retroarch.cfg rather than assuming a layout. Guessing goes wrong on Flatpak,
# Snap and portable installs, which all move them somewhere else.
#
#   ./install.sh                 install into the detected RetroArch
#   ./install.sh --dry-run       print what it would do
#   ./install.sh --config PATH   use a specific retroarch.cfg
set -e

DRY=0
CFG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY=1 ;;
    --config)  CFG="$2"; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
  shift
done

find_cfg() {
  for c in \
    "$HOME/.config/retroarch/retroarch.cfg" \
    "$HOME/.var/app/org.libretro.RetroArch/config/retroarch/retroarch.cfg" \
    "$SNAP_USER_COMMON/.config/retroarch/retroarch.cfg" \
    "$HOME/Library/Application Support/RetroArch/config/retroarch.cfg" \
    "$HOME/Library/Application Support/RetroArch/retroarch.cfg" \
    "./retroarch.cfg"
  do
    [ -f "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}

# Values are quoted in retroarch.cfg, and may be ":\dir" meaning relative to
# the config directory on a portable install.
cfg_get() {
  key=$1; file=$2
  line=$(grep -E "^[[:space:]]*$key[[:space:]]*=" "$file" 2>/dev/null | tail -1) || true
  [ -z "$line" ] && return 1
  val=$(printf '%s' "$line" | sed -E 's/^[^=]*=[[:space:]]*"?//; s/"?[[:space:]]*$//')
  [ -z "$val" ] && return 1
  case "$val" in
    ":"*) printf '%s/%s\n' "$(dirname "$file")" "$(printf '%s' "$val" | sed 's/^://; s/^[\\\/]//')" ;;
    *)    printf '%s\n' "$val" ;;
  esac
}

[ -z "$CFG" ] && CFG=$(find_cfg) || true
if [ -z "$CFG" ]; then
  echo "Could not find retroarch.cfg."
  echo "Run RetroArch once so it writes one, or pass --config PATH."
  echo "Failing that, copy aheui_libretro.so into the directory shown under"
  echo "Settings > Directory > Cores, and aheui_libretro.info under Core Info."
  exit 1
fi
echo "config:     $CFG"

CORES=$(cfg_get libretro_directory "$CFG" || true)
INFO=$(cfg_get libretro_info_path "$CFG" || true)
[ -z "$CORES" ] && CORES="$(dirname "$CFG")/cores"
[ -z "$INFO" ]  && INFO="$(dirname "$CFG")/info"
echo "cores:      $CORES"
echo "core info:  $INFO"

for f in aheui_libretro.so aheui_libretro.info; do
  [ -f "$f" ] || { echo "missing $f -- run make first"; exit 1; }
done

if [ "$DRY" = "1" ]; then
  echo
  echo "would copy aheui_libretro.so   -> $CORES/"
  echo "would copy aheui_libretro.info -> $INFO/"
  exit 0
fi

mkdir -p "$CORES" "$INFO"
cp aheui_libretro.so "$CORES/"
cp aheui_libretro.info "$INFO/"
echo
echo "installed."
echo
echo "Quickest check, bypassing the menus entirely:"
echo "  retroarch -L \"$CORES/aheui_libretro.so\" maze.png"
echo
echo "In the UI: Load Core > Aheui, then Load Content > pick a .png"
echo
echo "For anything that reads input (the adventures), turn on Game Focus --"
echo "Scroll Lock by default. Without it RetroArch keeps the keystrokes for"
echo "its own hotkeys and the program sits at its prompt looking hung."
