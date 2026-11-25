"""Whether this machine is in a state where a timed run means anything.

A Mac slows a program it can't show: with the screen locked or the display asleep, a
4 ms sleep can take 20, and every frame time measured then is the Mac's, not the game's.
The same goes for a processor held back by heat and for one busy compiling. Tools that
time a game ask here before and after a run, and keep the answer with their numbers.
Off the Mac, only what can be told is checked.
"""
import re
import subprocess
import sys


def screen_locked():
    """True with the Mac's screen locked."""
    if sys.platform != "darwin":
        return False
    import ctypes
    import ctypes.util
    graphics = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreGraphics"))
    foundation = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
    graphics.CGSessionCopyCurrentDictionary.restype = ctypes.c_void_p
    foundation.CFDictionaryGetValue.restype = ctypes.c_void_p
    foundation.CFDictionaryGetValue.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    foundation.CFStringCreateWithCString.restype = ctypes.c_void_p
    foundation.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
    foundation.CFBooleanGetValue.argtypes = [ctypes.c_void_p]
    session = graphics.CGSessionCopyCurrentDictionary()
    if not session:
        return False
    key = foundation.CFStringCreateWithCString(None, b"CGSSessionScreenIsLocked", 0x08000100)
    value = foundation.CFDictionaryGetValue(session, key)
    return bool(value) and bool(foundation.CFBooleanGetValue(value))


def _output(command):
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def display_on():
    """False with the Mac's display asleep. True where it can't be told."""
    if sys.platform != "darwin":
        return True
    found = re.search(r'"CurrentPowerState"=(\d+)', _output(["ioreg", "-r", "-d", "1", "-c", "IODisplayWrangler"]))
    return int(found.group(1)) >= 4 if found else True


def speed_limit():
    """The share of its speed the processor is allowed, 100 when it isn't held back."""
    found = re.search(r"CPU_Speed_Limit\s*=\s*(\d+)", _output(["pmset", "-g", "therm"])) if sys.platform == "darwin" else None
    return int(found.group(1)) if found else 100


def compilers_running():
    """How many compilers are running, as another session's build would be."""
    return len(_output(["pgrep", "-x", "clang"]).split())


def front_pid():
    """The process whose window is in front, or None where it can't be told."""
    if sys.platform != "darwin":
        return None
    found = re.search(r"\(in front\).*?^\s*pid = (\d+)", _output(["lsappinfo", "list"]), re.S | re.M)
    return int(found.group(1)) if found else None


def priority(pid):
    """The highest priority among a process's ordinary threads, or None where it can't be
    told. A program started by a background service inherits its low priority (20 on a
    Mac, where a program started from Terminal or the Finder gets 31 to 47), and then
    loses the processor to every other program: its frames wait while it isn't run."""
    if sys.platform != "darwin":
        return None
    rows = _output(["ps", "-M", "-p", str(pid)]).splitlines()[1:]
    found = [int(match.group(1)) for row in rows if (match := re.search(r"\s(\d+)T\s", row))]
    return max(found) if found else None


def conditions(pid=None):
    """The machine's state now. With a process ID, also whether that program is in front."""
    state = {"locked": screen_locked(), "display_on": display_on(), "speed_limit": speed_limit(), "compilers": compilers_running()}
    front = front_pid() if pid is not None else None
    if front is not None:  # Left out where it can't be told, rather than called not in front.
        state["in_front"] = front == pid
    if pid is not None and (level := priority(pid)) is not None:
        state["priority"] = level
    return state


def problems(state):
    """What in a state makes a timed run void, as plain sentences. Empty when nothing does."""
    found = []
    if state.get("locked"):
        found.append("the screen is locked")
    if not state.get("display_on", True):
        found.append("the display is asleep")
    if state.get("speed_limit", 100) < 100:
        found.append(f"the processor is held to {state['speed_limit']}% of its speed")
    if state.get("compilers"):
        found.append(f"{state['compilers']} compilers are running")
    if state.get("in_front") is False:
        found.append("the game's window isn't in front")
    if state.get("priority", 31) < 31:
        found.append(f"the game runs at low priority ({state['priority']}), as when a background service started it")
    return found


def describe(state):
    return "; ".join(problems(state)) or "fit for timing"


if __name__ == "__main__":
    now = conditions()
    print(describe(now))
    sys.exit(1 if problems(now) else 0)
