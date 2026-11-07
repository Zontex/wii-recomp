#!/bin/bash
# Turns a Raspberry Pi 5 into a Wii: plugged in, it starts straight into the Wii Menu on
# the TV, with nothing else on the screen. Run it from this repository, as the user who
# will own the console (not root; it asks for sudo), on Raspberry Pi OS:
#
#   recomp/tools/pi_setup.sh [--nand DIR] [--console DIR] [--channels] [--shop]
#                            [--keep-desktop | --developer] [--resolution 1920x1080@60] [--yes] [--dry-run]
#   recomp/tools/pi_setup.sh --only STEP[,STEP...]   just those steps, by the names below
#   recomp/tools/pi_setup.sh --undo                  boot to the desktop again
#
# It needs your Wii's storage in wii/nand first (README.md, "Add your Wii's storage").
# Each step can be run again; what's already done is kept. The steps, in order:
#
#   packages  the compilers and libraries the console builds with, and cage, which shows
#             one program full screen with no desktop around it
#   desktop   removes the desktop and its programs: the web browser, the office suite,
#             the login screen, printing and the like. It lists what goes and asks first
#             (--yes doesn't ask). --keep-desktop skips it. --undo can't bring it back.
#             --developer removes it but keeps labwc and grim, which test runs use to
#             drive the console on a screen of their own, with no TV, over ssh. The TV
#             still shows only the console; put it in standby before timing a test run.
#   sdl3      SDL3 for controllers (sdl3_setup.sh)
#   remotes   real Wii Remotes, the Balance Board and Switch controllers (remotes_setup.sh)
#   console   the console folder (default ~/wii-console), with its own copy of the storage
#   menu      the Wii Menu, translated and compiled (add_game.py)
#   channels  every channel in the storage, the same way (only with --channels; slow)
#   shop      the Wii Shop Channel's pictures and catalogue (only with --shop; needs the
#             internet once, to fetch them)
#   service   installs the console and its standby (pi_console/) as system services
#   boot      boots to the console: no login screen or boot messages, the TV's picture at
#             --resolution, the Pi's power button for the Wii's, Bluetooth always on
#
# The Pi then behaves as a Wii does. At power on the Wii Menu starts. Turning the Wii off,
# from the menu or a game, puts the Pi in standby: no picture, the processor at its
# slowest, Bluetooth listening. The Pi's power button, or the power button of a Wii
# Remote paired with it (sudo recomp/tools/remotes_setup.sh pair), turns it on again.
# Holding the Pi's button for three seconds shuts the Pi down. ssh keeps working, and
# `echo power | sudo tee /run/wii-recomp/control` presses the power button from there.
# The console's log is in <console>/logs/, and `journalctl -u wii-standby` says what the
# standby did. Games go in with add_game.py --console <console>, as on any computer.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAND=$ROOT/wii/nand
CONSOLE=$HOME/wii-console
RESOLUTION=1920x1080@60
CHANNELS=0 SHOP=0 DESKTOP=1 DEVELOPER=0 YES=0 DRY=0 UNDO=0 ONLY=
LIB=/usr/local/lib/wii-recomp
ETC=/etc/wii-recomp
UNITS=/etc/systemd/system
LOGIND=/etc/systemd/logind.conf.d/wii-recomp.conf
BOOT=/boot/firmware
BACKUP=.before-wii-recomp
MENU=00000001-00000002
PACKAGES="clang ninja-build python3 python3-cryptography pkg-config cmake git libglfw3-dev libvulkan-dev
  mesa-vulkan-drivers libshaderc-dev libasound2-dev libudev-dev libusb-1.0-0-dev cage bluez"
# Kept whatever the desktop's removal takes with it: sound, the network, ssh, Bluetooth.
KEEP="pipewire pipewire-alsa pipewire-pulse wireplumber network-manager openssh-server bluez
  raspberrypi-sys-mods rpi-eeprom $PACKAGES"
# The desktop and its programs, by name; only those installed are removed.
DESKTOP_PACKAGES="lightdm pi-greeter* rpd-* raspberrypi-ui-mods lxde* lxpanel* lxterminal pcmanfm wf-panel-pi
  labwc wayfire squeekboard chromium* rpi-chromium-mods firefox* libreoffice* thonny geany* mousepad galculator
  vlc* rpi-imager realvnc* rpi-connect* cups* system-config-printer* gnome-keyring* xcompmgr agnostics
  rp-prefapps scratch* sonic-pi* code nodered"
UNNEEDED_SERVICES="cups.service cups-browsed.service ModemManager.service triggerhappy.service packagekit.service
  colord.service rpi-connect.service rpi-connect-wayvnc.service"

usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --nand) NAND=$(realpath "$2"); shift ;;
    --console) CONSOLE=$(realpath -m "$2"); shift ;;
    --resolution) RESOLUTION=$2; shift ;;
    --channels) CHANNELS=1 ;;
    --shop) SHOP=1 ;;
    --keep-desktop) DESKTOP=0 ;;
    --developer) DEVELOPER=1 ;;
    --yes) YES=1 ;;
    --dry-run) DRY=1 ;;
    --only) ONLY=",$2,"; shift ;;
    --undo) UNDO=1 ;;
    -h|--help) usage ;;
    *) echo "pi_setup: unknown option $1" >&2; usage 2 ;;
  esac
  shift
done

say() { printf '\n== %s\n' "$*"; }
note() { printf '   %s\n' "$*"; }
run() { if [ "$DRY" = 1 ]; then printf '   would run: %s\n' "$*"; else "$@"; fi; }
# Writes stdin to a root-owned file.
put() {
  if [ "$DRY" = 1 ]; then
    printf '   would write %s:\n' "$1"
    if [ -f "$1" ]; then diff -u "$1" - | tail -n +3 | grep '^[-+]' | sed 's/^/     /' || true; else sed 's/^/     | /'; fi
    return
  fi
  sudo mkdir -p "$(dirname "$1")"
  sudo tee "$1" > /dev/null
}
# A step runs when --only names it, or, without --only, unless its option leaves it out.
wanted() {
  if [ -n "$ONLY" ]; then [[ $ONLY == *",$1,"* ]]; return; fi
  case "$1" in channels) [ "$CHANNELS" = 1 ] ;; shop) [ "$SHOP" = 1 ] ;; desktop) [ "$DESKTOP" = 1 ] ;; *) true ;; esac
}
installed() { dpkg-query -W -f='${Status}\n' "$1" 2>/dev/null | grep -q '^install ok installed'; }
unit_exists() { systemctl list-unit-files "$1" --no-legend 2>/dev/null | grep -q .; }

check() {
  [ "$(uname -m)" = aarch64 ] || { echo "pi_setup: this is for a Raspberry Pi 5 (64-bit Raspberry Pi OS)" >&2; exit 1; }
  grep -q "Raspberry Pi 5" /proc/device-tree/model 2>/dev/null || note "this isn't a Raspberry Pi 5: $(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
  [ "$(id -u)" != 0 ] || { echo "pi_setup: run it as the console's user, not root; it asks for sudo" >&2; exit 1; }
  [ -f /etc/debian_version ] || { echo "pi_setup: this needs Raspberry Pi OS (or Debian)" >&2; exit 1; }
  if [ "$UNDO" = 0 ] && { wanted console || wanted menu || wanted channels; } && [ ! -d "$NAND/title" ]; then
    echo "pi_setup: no Wii storage in $NAND. Put your NAND backup in wii/ and run recomp/tools/import_nand.py (README.md)," >&2
    echo "or give its folder with --nand." >&2
    exit 1
  fi
  local step
  for step in packages desktop sdl3 remotes service boot; do  # The steps that change the system.
    if [ "$DRY" = 0 ] && { [ "$UNDO" = 1 ] || wanted "$step"; }; then sudo -v; break; fi
  done
}

step_packages() {
  say "Packages the console needs"
  run sudo apt-get update
  # shellcheck disable=SC2086
  run sudo apt-get install -y $PACKAGES
  local groups=""
  for group in video render input audio bluetooth; do
    getent group "$group" > /dev/null && ! id -nG | tr ' ' '\n' | grep -qx "$group" && groups="$groups,$group"
  done
  [ -z "$groups" ] || run sudo usermod -aG "${groups#,}" "$USER"
}

step_desktop() {
  say "The desktop and the programs the console doesn't need"
  local remove=() pattern name keep=$KEEP
  [ "$DEVELOPER" = 0 ] || { keep="$keep labwc grim"; note "keeping labwc and grim for test runs (--developer)"; }
  for pattern in $DESKTOP_PACKAGES; do
    while read -r name; do
      [ -n "$name" ] && installed "$name" && ! [[ " $keep " == *" $name "* ]] && remove+=("$name")
    done < <(dpkg-query -W -f='${Package}\n' "$pattern" 2>/dev/null || true)
  done
  if [ ${#remove[@]} = 0 ]; then note "nothing left to remove"; return; fi
  note "removing: ${remove[*]}"
  # shellcheck disable=SC2086
  local kept=$(for name in $keep; do installed "$name" && echo "$name"; done)
  if [ "$DRY" = 1 ]; then
    note "apt would remove, with what only they needed:"
    apt-get -s purge "${remove[@]}" 2>/dev/null | grep '^Purg' | awk '{print $2}' | tr '\n' ' ' | fold -s -w 100 | sed 's/^/     /'
    echo
    return
  fi
  if [ "$YES" = 0 ]; then
    read -r -p "   Remove these and what only they needed? This can't be undone by --undo. [y/N] " answer
    [[ $answer == [yY]* ]] || { note "kept the desktop"; return; }
  fi
  # shellcheck disable=SC2086
  [ -z "$kept" ] || sudo apt-mark manual $kept > /dev/null
  # labwc's settings, which test runs copy, belong to the desktop's package: kept for --developer.
  local settings=
  if [ "$DEVELOPER" = 1 ] && [ -d /etc/xdg/labwc ]; then settings=$(mktemp -d); cp -a /etc/xdg/labwc "$settings/"; fi
  sudo apt-get purge -y "${remove[@]}"
  sudo apt-get autoremove --purge -y
  if [ -n "$settings" ]; then
    [ -d /etc/xdg/labwc ] || sudo cp -a "$settings/labwc" /etc/xdg/labwc
    rm -rf "$settings"
  fi
  sudo apt-get clean
}

step_sdl3() {
  say "SDL3, for controllers"
  if [ -f /usr/local/include/SDL3/SDL.h ] || [ -f "$HOME/.local/include/SDL3/SDL.h" ]; then note "already built"; return; fi
  run "$ROOT/recomp/tools/sdl3_setup.sh"
}

step_remotes() {
  say "Real Wii Remotes, the Balance Board and Switch controllers"
  run sudo "$ROOT/recomp/tools/remotes_setup.sh"
}

step_console() {
  say "The console folder, $CONSOLE"
  if [ -d "$CONSOLE/nand" ]; then note "already there; its storage is kept"; return; fi
  run python3 "$ROOT/recomp/tools/prepare_console.py" "$NAND" "$CONSOLE"
}

step_menu() {
  say "The Wii Menu, translated and compiled for this Pi"
  run python3 "$ROOT/recomp/tools/add_game.py" "$MENU" --nand "$NAND" --console "$CONSOLE"
}

step_channels() {
  say "The channels in the storage"
  local failed=() folder key
  for folder in "$NAND"/title/0001000{1,2,8}/*; do
    [ -f "$folder/content/title.tmd" ] || continue
    key=$(basename "$(dirname "$folder")")-$(basename "$folder")
    note "$key"
    run python3 "$ROOT/recomp/tools/add_game.py" "$key" --nand "$NAND" --console "$CONSOLE" || failed+=("$key")
  done
  [ ${#failed[@]} = 0 ] || note "these didn't build, and the menu will say it can't start them: ${failed[*]}"
}

step_shop() {
  say "The Wii Shop Channel's pictures and catalogue"
  local sources=$ROOT/wii/shop/sources/WiiMart
  if [ ! -d "$sources" ]; then
    run git clone --depth 1 --filter=blob:none --sparse https://github.com/WiiMart/WiiMart.git "$sources"
    run git -C "$sources" sparse-checkout set www/oss/oss/common
  fi
  run python3 "$ROOT/recomp/tools/shop/assets.py" "$sources"
}

step_service() {
  say "The console and its standby, as system services"
  [ -x "$ROOT/recomp/build/wii-recomp" ] || [ "$DRY" = 1 ] || { echo "pi_setup: no console in recomp/build; run the menu step first" >&2; exit 1; }
  run sudo install -d "$LIB" "$ETC"
  run sudo install -m 755 "$ROOT"/recomp/tools/pi_console/{standby.py,session.sh,stop.sh} "$LIB/"
  put "$ETC/console.conf" <<EOF
# The console pi_setup.sh set up. session.sh reads it.
USER=$USER
CONSOLE=$CONSOLE
PROGRAM=$ROOT/recomp/build/wii-recomp
EOF
  [ -f "$ETC/session.env" ] || put "$ETC/session.env" <<'EOF'
# Settings for the console program, one NAME=value a line, such as WII_RECOMP_SCALE=3/2
# (recomp/README.md lists them). Read when the console turns on.
EOF
  put "$UNITS/wii-console.service" <<EOF
# The console on the TV: cage shows it full screen on the first screen, in a session of
# its own, as the desktop's login would start one. Started and stopped by wii-standby.
[Unit]
Description=Wii Recomp console
After=systemd-user-sessions.service plymouth-quit-wait.service bluetooth.service
Conflicts=getty@tty1.service

[Service]
User=$USER
PAMName=login
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes
TTYVTDisallocate=yes
StandardInput=tty-fail
StandardOutput=journal
StandardError=journal
UtmpIdentifier=tty1
UtmpMode=user
RuntimeDirectory=wii-console
RuntimeDirectoryPreserve=yes
Environment=XDG_SESSION_TYPE=wayland
EnvironmentFile=-$ETC/session.env
ExecStart=/usr/bin/cage -- $LIB/session.sh
ExecStop=$LIB/stop.sh
TimeoutStopSec=25
EOF
  put "$UNITS/wii-standby.service" <<EOF
# The power light: turns the console on at boot, and keeps the Pi in standby when the
# Wii is turned off, until its power button or a paired remote turns it on again.
[Unit]
Description=Wii Recomp standby
After=bluetooth.service

[Service]
ExecStart=/usr/bin/python3 $LIB/standby.py
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
  run sudo systemctl daemon-reload
}

edit_cmdline() {
  local file=$BOOT/cmdline.txt words resolution=$RESOLUTION
  [ -f "$file" ] || { note "no $file; the boot messages and the TV's mode stay as they are"; return; }
  [ -f "$file$BACKUP" ] || run sudo cp "$file" "$file$BACKUP"
  local original=$file$BACKUP
  [ -f "$original" ] || original=$file  # A dry run made no copy.
  words=$(tr -s ' \n' ' ' < "$original" | sed 's/^ //; s/ $//')
  local kept=() word
  for word in $words; do
    case "$word" in console=tty1|quiet|loglevel=*|logo.nologo|vt.global_cursor_default=*|video=HDMI-A-*) ;; *) kept+=("$word") ;; esac
  done
  kept+=(console=tty3 quiet loglevel=3 logo.nologo vt.global_cursor_default=0 "video=HDMI-A-1:$resolution" "video=HDMI-A-2:$resolution")
  echo "${kept[*]}" | put "$file"
}

edit_config() {
  local file=$BOOT/config.txt
  [ -f "$file" ] || return 0
  grep -q '^disable_splash=1' "$file" && return
  [ -f "$file$BACKUP" ] || run sudo cp "$file" "$file$BACKUP"
  { cat "$file"; printf '\n[all]\n# No rainbow square at power on (pi_setup.sh)\ndisable_splash=1\n'; } | put "$file"
}

edit_bluetooth() {
  local file=/etc/bluetooth/main.conf
  [ -f "$file" ] || return 0
  grep -q '^FastConnectable *= *true' "$file" && grep -q '^AutoEnable *= *true' "$file" && return
  [ -f "$file$BACKUP" ] || run sudo cp "$file" "$file$BACKUP"
  # Remotes reconnect sooner, and the adapter is on from boot, with nobody logged in.
  sed -e '/^#* *FastConnectable *=/d' -e '/^#* *AutoEnable *=/d' \
      -e 's/^\[General\]$/[General]\nFastConnectable = true/' -e 's/^\[Policy\]$/[Policy]\nAutoEnable = true/' "$file" | put "$file"
}

step_boot() {
  say "Boot straight to the console"
  put "$LOGIND" <<'EOF'
# The Pi's power button is the Wii's: wii-standby reads it (pi_setup.sh).
[Login]
HandlePowerKey=ignore
HandlePowerKeyLongPress=ignore
EOF
  edit_cmdline
  edit_config
  edit_bluetooth
  local service
  for service in $UNNEEDED_SERVICES; do
    unit_exists "$service" && run sudo systemctl disable --now "$service" 2>/dev/null || true
  done
  unit_exists lightdm.service && run sudo systemctl disable lightdm.service
  run sudo systemctl set-default multi-user.target
  run sudo systemctl enable bluetooth.service wii-standby.service
  note "restart the Pi (sudo reboot) and it starts into the Wii Menu"
}

undo() {
  say "Boot to the desktop again"
  run sudo systemctl disable --now wii-standby.service || true
  run sudo systemctl stop wii-console.service || true
  run sudo rm -f "$UNITS/wii-standby.service" "$UNITS/wii-console.service" "$LOGIND"
  run sudo rm -rf "$LIB"
  local file
  for file in "$BOOT/cmdline.txt" "$BOOT/config.txt" /etc/bluetooth/main.conf; do
    [ -f "$file$BACKUP" ] && run sudo mv "$file$BACKUP" "$file"
  done
  run sudo systemctl daemon-reload
  if unit_exists lightdm.service; then
    run sudo systemctl enable lightdm.service
    run sudo systemctl set-default graphical.target
  else
    note "the desktop was removed; sudo apt install raspberrypi-ui-mods brings it back"
  fi
  note "$ETC and the console folder ($CONSOLE) are kept"
}

check
if [ "$UNDO" = 1 ]; then undo; exit; fi
[ "$DRY" = 0 ] || note "dry run: nothing is changed"
for step in packages desktop sdl3 remotes console menu channels shop service boot; do
  wanted "$step" && "step_$step"
done
say "Done"
