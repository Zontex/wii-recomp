#!/bin/sh
# Sets up a Linux machine, such as a Raspberry Pi, for real Wii Remotes, the Balance Board
# and Switch controllers. Run it with sudo.
#
#   remotes_setup.sh                       set up: remotes through the kernel's HID
#   remotes_setup.sh pair                  pair a remote or board: press SYNC first
#   remotes_setup.sh --sockets [--program recomp/build/wii-recomp]
#                                          remotes over raw sockets instead (see below)
#   remotes_setup.sh --undo [--program ...]  put everything back
#
# Setting up does two things, and prints each:
#   1. Keeps the kernel's own Wii Remote driver (hid-wiimote) from loading. bluetoothd
#      takes a remote's connection as it does any controller's, the kernel gives it a
#      hidraw device, and Wii Recomp reads it there (runtime/remote_link_hidraw.c). The
#      driver would send the remote reports of its own, so it's kept away.
#   2. Lets anyone at the machine, and the input group, read Nintendo's controllers
#      through hidraw: Wii Remotes and boards, and Switch controllers, which SDL then reads
#      itself, with their gyro and rumble (runtime/pads.c). Switch 2 controllers by cable
#      need their USB devices too.
# bluetoothd's input plugin stays on, so Bluetooth keyboards, mice and wireless Switch
# controllers keep working beside the remotes.
#
# --sockets is the older way: bluetoothd starts without its input plugin, so it no longer
# takes the HID channels (PSM 0x11 and 0x13), and Wii Recomp answers remotes that call
# over raw sockets, as a Wii does (runtime/remote_link_linux.c). The program needs
# cap_net_bind_service for that, so run it again after each build. Bluetooth keyboards,
# mice and wireless Switch controllers stop working while it's set.
#
# `pads` is kept as another name for setting up.
set -eu

DROP_IN=/etc/systemd/system/bluetooth.service.d/wii-recomp.conf
RULES=/etc/udev/rules.d/71-wii-recomp-pads.rules
BLACKLIST=/etc/modprobe.d/wii-recomp.conf
PROGRAM=recomp/build/wii-recomp
ACTION=setup
while [ $# -gt 0 ]; do
  case "$1" in
    --undo) ACTION=undo ;;
    --sockets) ACTION=sockets ;;
    --program) PROGRAM="$2"; shift ;;
    pair) ACTION=pair ;;
    pads) ACTION=setup ;;
    *) echo "usage: $0 [--sockets | --undo] [--program PATH] | pair" >&2; exit 2 ;;
  esac
  shift
done

if [ "$(id -u)" != 0 ]; then
  echo "run this with sudo" >&2
  exit 1
fi

find_bluetoothd() {
  for candidate in /usr/libexec/bluetooth/bluetoothd /usr/lib/bluetooth/bluetoothd /usr/sbin/bluetoothd; do
    if [ -x "$candidate" ]; then echo "$candidate"; return; fi
  done
  echo "bluetoothd isn't installed: sudo apt install bluez" >&2
  exit 1
}

restart_bluetooth() {
  systemctl daemon-reload
  systemctl restart bluetooth
}

# Nintendo's controllers, by Bluetooth (the HID name the kernel gives them) or by cable
# (the USB IDs, SDL's src/joystick/usb_ids.h: Switch Joy-Cons, Pro Controller, Switch 2's).
write_rules() {
  cat > "$RULES" <<'RULES'
KERNEL=="hidraw*", KERNELS=="*057E:*", MODE="0660", GROUP="input", TAG+="uaccess"
KERNEL=="hidraw*", ATTRS{idVendor}=="057e", MODE="0660", GROUP="input", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="057e", ATTRS{idProduct}=="2006|2007|2008|2009|200e|2066|2067|2068|2069|2073", MODE="0660", GROUP="input", TAG+="uaccess"
RULES
  udevadm control --reload-rules
  udevadm trigger
}

case "$ACTION" in
  setup)
    echo "1. The kernel's Wii Remote driver stays unloaded ($BLACKLIST)"
    echo "blacklist hid_wiimote" > "$BLACKLIST"
    modprobe -r hid_wiimote 2>/dev/null || true
    echo "2. Nintendo's controllers can be read through hidraw ($RULES)"
    write_rules
    if [ -f "$DROP_IN" ]; then
      echo "   bluetoothd gets its input plugin back, from the older --sockets setup"
      rm -f "$DROP_IN"
      restart_bluetooth
      setcap -r "$PROGRAM" 2>/dev/null || true
    fi
    echo "Done. Pair each remote and the board once with: sudo $0 pair"
    echo "Pair a wireless Switch controller with bluetoothctl: hold its sync button, then scan on, pair, trust and connect."
    ;;
  sockets)
    BLUETOOTHD=$(find_bluetoothd)
    echo "1. bluetoothd starts without its input plugin ($DROP_IN)"
    mkdir -p "$(dirname "$DROP_IN")"
    printf '[Service]\nExecStart=\nExecStart=%s --noplugin=input\n' "$BLUETOOTHD" > "$DROP_IN"
    restart_bluetooth
    echo "2. $PROGRAM may listen for remotes (cap_net_bind_service)"
    setcap cap_net_bind_service+ep "$PROGRAM"
    echo "Done. Wireless Switch controllers won't connect while this is set; $0 sets up the HID way instead."
    ;;
  pair)
    # BlueZ answers the PIN for SYNC itself: it's this machine's own address. A remote
    # paired this way calls the console with any button from then on.
    echo "Press SYNC on the remote (under the battery cover) or the board (in its battery bay)."
    bluetoothctl --timeout 15 scan on > /dev/null 2>&1 &
    sleep 12
    found=$(bluetoothctl devices | grep -E "Nintendo RVL-(CNT|WBC)" || true)
    if [ -z "$found" ]; then
      echo "Nothing found. Press SYNC again and rerun this." >&2
      exit 1
    fi
    echo "$found" | while read -r _ address name; do
      echo "Pairing $name ($address)"
      bluetoothctl pair "$address" && bluetoothctl trust "$address" || echo "Couldn't pair $address: press SYNC and try again" >&2
      if [ -f "$DROP_IN" ]; then
        bluetoothctl disconnect "$address" > /dev/null 2>&1 || true  # Over sockets, Wii Recomp connects to it.
      else
        bluetoothctl connect "$address" > /dev/null 2>&1 || true  # Through HID, bluetoothd keeps the connection.
      fi
    done
    ;;
  undo)
    echo "1. The kernel's Wii Remote driver may load again"
    rm -f "$BLACKLIST"
    echo "2. Nintendo's controllers are back to the system's permissions"
    rm -f "$RULES"
    udevadm control --reload-rules 2>/dev/null || true
    if [ -f "$DROP_IN" ]; then
      echo "3. bluetoothd starts as before"
      rm -f "$DROP_IN"
      restart_bluetooth
    fi
    setcap -r "$PROGRAM" 2>/dev/null || true
    ;;
esac
