#!/usr/bin/env python3
"""The Raspberry Pi's power light, as a Wii's: on, or standby with Bluetooth listening.

Installed by recomp/tools/pi_setup.sh and run by systemd (wii-standby.service) as root.
It turns the console on at boot by starting wii-console.service, the Wii Menu on the TV.
When the menu or a game turns the Wii off, the console program ends and the Pi goes to
standby rather than shutting down: the screen has no picture, the processor slows to
its lowest speed, and Bluetooth keeps listening. From there the console turns on again
when

  - the Pi's own power button is pressed, or
  - a Wii Remote paired with the Pi (remotes_setup.sh pair) calls it, which is what a
    paired remote does when its power button is pressed, or
  - "power" is written to /run/wii-recomp/control (for instance over ssh).

While the console is on, a press of the Pi's power button presses the Wii's: the program
turns off its own way, saving what it needs to, and the Pi goes to standby. Holding the
Pi's button for three seconds, or writing "off", shuts the Pi down for real; pressing
its button then starts it again, straight into the Wii Menu.

The console's settings come from /etc/wii-recomp/console.conf (USER, CONSOLE, PROGRAM).
With --user it drives the user's own services (systemctl --user) and keeps its control
file in $XDG_RUNTIME_DIR, which tries it out without root: the power button and the
remotes' connections need root to read and drop, the control file still works.
"""
import glob
import os
import queue
import struct
import subprocess
import sys
import threading
import time

SESSION = "wii-console.service"
USER_MODE = "--user" in sys.argv[1:]
RUNTIME = os.environ.get("XDG_RUNTIME_DIR", "/run") if USER_MODE else "/run"
RUN = RUNTIME + "/wii-recomp"
CONTROL = RUN + "/control"
EXIT_FILE = RUNTIME + "/wii-console/exit"  # Written by session.sh: the console program's exit code.
EVENT = struct.Struct("llHHi")  # struct input_event on a 64-bit kernel: time, type, code, value.
EV_KEY, KEY_POWER = 1, 116
HOLD_TO_SHUT_DOWN = 3.0  # Seconds.
NINTENDO_BLUETOOTH = "0005:0000057E:"  # HID_ID of a Nintendo controller connected by Bluetooth.
CRASHES, CRASH_WINDOW = 3, 120  # This many ends that aren't a power-off, this close, and it waits in standby.

events = queue.Queue()


def log(message):
    print(f"standby: {message}", flush=True)


def systemctl(*arguments):
    return subprocess.run(["systemctl", *(["--user"] if USER_MODE else []), *arguments], capture_output=True, text=True)


def session_active():
    return systemctl("is-active", SESSION).stdout.strip() in ("active", "activating", "deactivating")


def last_exit():
    try:
        with open(EXIT_FILE) as source:
            return int(source.read().strip())
    except (OSError, ValueError):
        return None


def set_governor(name):
    for path in glob.glob("/sys/devices/system/cpu/cpufreq/policy*/scaling_governor"):
        try:
            with open(path, "w") as out:
                out.write(name)
        except OSError:
            pass


def nintendo_controllers():
    """{hidraw name: Bluetooth address} for Nintendo controllers connected by Bluetooth."""
    found = {}
    for uevent in glob.glob("/sys/class/hidraw/hidraw*/device/uevent"):
        try:
            with open(uevent) as source:
                fields = dict(line.rstrip("\n").split("=", 1) for line in source if "=" in line)
        except OSError:
            continue
        if fields.get("HID_ID", "").upper().startswith(NINTENDO_BLUETOOTH):
            found[uevent.split("/")[4]] = fields.get("HID_UNIQ", "")
    return found


def disconnect_controllers():
    """Drops the remotes' connections, which turns them off, as a Wii going to standby does."""
    for address in sorted(set(nintendo_controllers().values())):
        if address:
            subprocess.run(["bluetoothctl", "disconnect", address], capture_output=True, timeout=10)
            log(f"disconnected {address}")
    for _ in range(30):  # Up to 3 s for them to go, so one still leaving isn't taken as one calling.
        if not nintendo_controllers():
            return
        time.sleep(0.1)


def power_button_device():
    for name_path in glob.glob("/sys/class/input/event*/device/name"):
        try:
            with open(name_path) as source:
                if source.read().strip() == "pwr_button":
                    return "/dev/input/" + name_path.split("/")[4]
        except OSError:
            pass
    return None


def watch_power_button():
    """A short press is "power", holding it is "off"."""
    while True:
        device = power_button_device()
        if not device:
            log("no power button found (pwr_button); only remotes and the control file turn the console on")
            return
        try:
            with open(device, "rb") as source:
                pressed = None
                while True:
                    data = source.read(EVENT.size)
                    if len(data) < EVENT.size:
                        break
                    _, _, kind, code, value = EVENT.unpack(data)
                    if kind != EV_KEY or code != KEY_POWER:
                        continue
                    if value == 1:
                        pressed = time.monotonic()
                    elif value == 0 and pressed is not None:
                        held, pressed = time.monotonic() - pressed, None
                        events.put("off" if held >= HOLD_TO_SHUT_DOWN else "power")
        except OSError as error:
            log(f"power button: {error}")
        time.sleep(1)


def watch_control():
    """Lines written to the control file, as from `echo power > /run/wii-recomp/control`."""
    os.makedirs(RUN, exist_ok=True)
    if os.path.exists(CONTROL):
        os.unlink(CONTROL)
    os.mkfifo(CONTROL, 0o620)
    while True:
        with open(CONTROL) as source:
            for line in source:
                word = line.strip()
                if word in ("power", "on", "standby", "off"):
                    events.put(word)
                elif word:
                    log(f"control: {word!r} isn't power, on, standby or off")


def next_event(timeout):
    try:
        return events.get(timeout=timeout)
    except queue.Empty:
        return None


def turn_on():
    set_governor("ondemand")
    try:
        os.unlink(EXIT_FILE)
    except OSError:
        pass
    result = systemctl("start", SESSION)
    if result.returncode:
        log(f"couldn't start {SESSION}: {result.stderr.strip()}")
        return False
    log("on")
    return True


def stop_session():
    systemctl("stop", SESSION)  # Its ExecStop presses the Wii's power button and waits.


def shut_down():
    log("shutting the Pi down")
    stop_session()
    if not USER_MODE:
        systemctl("poweroff")
    sys.exit(0)


def run_while_on():
    """Waits until the console turns off. Returns True for a power-off, False for a crash."""
    while True:
        word = next_event(1.0)
        if word == "off":
            shut_down()
        if word in ("power", "standby"):
            log("the power button: turning the console off")
            stop_session()
        if not session_active():
            code = last_exit()
            if code == 0:
                return True
            log(f"the console ended without turning off (exit {code}); see its logs folder")
            return False


def standby():
    """Waits in standby for the power button or a remote."""
    disconnect_controllers()
    set_governor("powersave")
    log("waiting for the power button or a remote")
    while True:
        word = next_event(0.5)
        if word == "off":
            shut_down()
        if word in ("power", "on"):
            log("turned on by the power button")
            return
        calling = nintendo_controllers()
        if calling:
            log(f"turned on by a controller calling ({', '.join(sorted(set(calling.values())))})")
            return


def main():
    threading.Thread(target=watch_power_button, daemon=True).start()
    threading.Thread(target=watch_control, daemon=True).start()
    crashes = []
    while True:
        if not turn_on():
            standby()
            continue
        if run_while_on():
            crashes.clear()
            standby()
            continue
        now = time.monotonic()
        crashes = [when for when in crashes if now - when < CRASH_WINDOW] + [now]
        if len(crashes) >= CRASHES:
            log(f"{CRASHES} crashes in {CRASH_WINDOW} s: waiting in standby rather than starting again")
            crashes.clear()
            standby()
        else:
            time.sleep(2)


if __name__ == "__main__":
    main()
