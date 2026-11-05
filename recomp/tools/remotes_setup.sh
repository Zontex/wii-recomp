#!/bin/sh
# Sets up a Linux machine, such as a Raspberry Pi, for real Wii Remotes and the Balance
# Board (recomp/runtime/remote_link_linux.c). Run it with sudo.
#
#   remotes_setup.sh [--program recomp/build/wii-recomp]   set up
#   remotes_setup.sh pair                                  pair a remote or board: press SYNC first
#   remotes_setup.sh pads                                  let SDL read Switch controllers directly
#   remotes_setup.sh --undo [--program ...]                put everything back
#
# pads adds a udev rule so the console reads Switch controllers (and Switch 2 ones by
# cable) through SDL's own driver, which has their gyro and rumble, rather than the
# kernel's. Wireless Switch controllers connect through bluetoothd's input plugin, which
# setting up for remotes turns off: until remotes are read another way, use either real
# remotes or wireless Switch controllers on Linux, or Switch controllers by cable.
#
# Setting up does two things, and prints each:
#   1. Starts bluetoothd without its input plugin, so it no longer takes the HID channels
#      (PSM 0x11 and 0x13), and Wii Recomp answers remotes that call the console, as a Wii
#      does. Bluetooth keyboards and mice stop working while this is set.
#   2. Lets the program listen on those channels. Linux keeps channels below 0x1001 for
#      privileged programs. Rebuilding replaces the program, so run this again after.
set -eu

DROP_IN=/etc/systemd/system/bluetooth.service.d/wii-recomp.conf
PAD_RULES=/etc/udev/rules.d/71-wii-recomp-pads.rules
PROGRAM=recomp/build/wii-recomp
ACTION=setup
while [ $# -gt 0 ]; do
  case "$1" in
    --undo) ACTION=undo ;;
    --program) PROGRAM="$2"; shift ;;
    pair) ACTION=pair ;;
    pads) ACTION=pads ;;
    *) echo "usage: $0 [--undo] [--program PATH] | pair | pads" >&2; exit 2 ;;
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

case "$ACTION" in
  setup)
    BLUETOOTHD=$(find_bluetoothd)
    echo "1. bluetoothd starts without its input plugin ($DROP_IN)"
    mkdir -p "$(dirname "$DROP_IN")"
    printf '[Service]\nExecStart=\nExecStart=%s --noplugin=input\n' "$BLUETOOTHD" > "$DROP_IN"
    systemctl daemon-reload
    systemctl restart bluetooth
    echo "2. $PROGRAM may listen for remotes (cap_net_bind_service)"
    setcap cap_net_bind_service+ep "$PROGRAM"
    echo "Done. Pair each remote and the board once with: sudo $0 pair"
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
      bluetoothctl disconnect "$address" > /dev/null 2>&1 || true  # Wii Recomp connects to it, not bluetoothd.
    done
    ;;
  pads)
    # Nintendo's controllers, by cable (the USB IDs) or Bluetooth (the HID name the kernel
    # gives them), to anyone at the machine and the input group. The product IDs are SDL's
    # (src/joystick/usb_ids.h): Switch Joy-Cons, Pro Controller and the Switch 2's.
    echo "1. Switch controllers can be read by SDL ($PAD_RULES)"
    cat > "$PAD_RULES" <<'RULES'
KERNEL=="hidraw*", ATTRS{idVendor}=="057e", MODE="0660", GROUP="input", TAG+="uaccess"
KERNEL=="hidraw*", KERNELS=="*057E:*", MODE="0660", GROUP="input", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="057e", ATTRS{idProduct}=="2006|2007|2008|2009|200e|2066|2067|2068|2069|2073", MODE="0660", GROUP="input", TAG+="uaccess"
RULES
    udevadm control --reload-rules
    udevadm trigger
    if [ -f "$DROP_IN" ]; then
      echo "Note: bluetoothd's input plugin is off for real remotes, so Switch controllers work by cable only. $0 --undo turns it back on."
    fi
    echo "Done. Pair a wireless controller with bluetoothctl: hold its sync button, then scan on, pair, trust and connect."
    ;;
  undo)
    rm -f "$PAD_RULES"
    udevadm control --reload-rules 2>/dev/null || true
    echo "1. bluetoothd starts as before"
    rm -f "$DROP_IN"
    systemctl daemon-reload
    systemctl restart bluetooth
    echo "2. $PROGRAM no longer listens for remotes"
    setcap -r "$PROGRAM" 2>/dev/null || true
    ;;
esac
