#!/usr/bin/env python3
"""Write the ninja file that compiles the translated game.

Usage: recomp/translate/build_file.py recomp/build [backend,backend]

Backends are the graphics APIs to build in, the first being the default at run
time. WII_RECOMP_RENDERER picks among them. Without a list, each platform's usual ones,
leaving Vulkan out where its libraries aren't installed. VULKAN_SDK names a folder
holding the Vulkan libraries and headers, if they aren't installed system-wide.
"""
import os
import sys
from glob import glob
from pathlib import Path

VULKAN_SDK = os.environ.get("VULKAN_SDK")
# Arm's C leaves char unsigned and merges multiply-adds, both unlike the Intel Mac.
FLAGS = "-w -fno-strict-aliasing -fwrapv -fsigned-char -ffp-contract=off -I/usr/local/include" + (f" -I{VULKAN_SDK}/include" if VULKAN_SDK else "")
GAME_OPTIMIZATION = "-O2"
RUNTIME_OPTIMIZATION = "-O3"
if sys.platform == "linux" and os.uname().machine == "aarch64":
    FLAGS += " -mcpu=cortex-a76"  # The Raspberry Pi 5.
COMMON_LINK_FLAGS = "-L/usr/local/lib -lglfw -lm -lpthread"
VULKAN_LINK_FLAGS = (f"-L{VULKAN_SDK}/lib -Wl,-rpath,{VULKAN_SDK}/lib " if VULKAN_SDK else "") + "-lvulkan"
SHADERC = "shaderc" if sys.platform == "linux" else "shaderc_shared"  # Debian's name for it.
VULKAN_LINK_FLAGS += f" -l{SHADERC}"

# For each platform, what it always links, and what each backend adds. The Pi draws at
# 5/2, which puts the game's 456 lines above a full 1080p picture.
PLATFORMS = {
    "darwin": {"system": "-framework AudioToolbox", "opengl": "-framework OpenGL", "vulkan": VULKAN_LINK_FLAGS, "default": ["opengl", "vulkan"]},
    "linux": {"defines": "-DEFB_SCALE=5/2", "system": "-lasound", "opengl": "-lGL", "vulkan": VULKAN_LINK_FLAGS, "default": ["vulkan"]},
}
AUDIO_SOURCES = {"darwin": "audio_output_mac.c", "linux": "audio_output_linux.c"}
BACKEND_SOURCES = {"opengl": ["gx_gl.c"], "vulkan": ["gx_vulkan.c", "gx_vulkan_device.c", "gx_vulkan_pipeline.c"]}
LIBRARY_FOLDERS = ["/usr/local/lib", "/usr/lib", "/usr/lib/aarch64-linux-gnu"] + ([f"{VULKAN_SDK}/lib"] if VULKAN_SDK else [])
VULKAN_LIBRARIES = ["vulkan", SHADERC]


def has_library(name):
    return any(glob(f"{folder}/lib{name}.*") for folder in LIBRARY_FOLDERS)


def default_backends(platform):
    wanted = platform["default"]
    missing = [name for name in VULKAN_LIBRARIES if not has_library(name)]
    if "vulkan" in wanted and missing and len(wanted) > 1:
        print("leaving Vulkan out, as these aren't installed:", ", ".join(missing))
        return [backend for backend in wanted if backend != "vulkan"]
    return wanted


def main():
    build = Path(sys.argv[1])
    generated = sorted((build / "generated").glob("*.c"))
    platform = PLATFORMS[sys.platform]
    backends = sys.argv[2].split(",") if len(sys.argv) > 2 else default_backends(platform)
    link_flags = " ".join([COMMON_LINK_FLAGS, platform["system"]] + [platform[backend] for backend in backends])
    defines = " ".join(f"-DGX_WITH_{backend.upper()}" for backend in backends) + " " + platform.get("defines", "")
    left_out = {name for backend, names in BACKEND_SOURCES.items() if backend not in backends for name in names}
    left_out |= {name for system, name in AUDIO_SOURCES.items() if system != sys.platform}
    lines = [f"cflags = {FLAGS}", "",
             "rule cc", "  command = clang $cflags -c $in -o $out", "  description = $in", "",
             # The runtime's headers change, and files that include them must follow.
             "rule runtime_cc", "  command = clang $cflags -MMD -MF $out.d -c $in -o $out", "  description = $in",
             "  depfile = $out.d", "  deps = gcc", "",
             "rule link", f"  command = clang $in -o $out {link_flags}", ""]
    objects = []
    for source in generated:
        target = f"obj/{source.stem}.o"
        lines.append(f"build {target}: cc generated/{source.name}\n  cflags = {GAME_OPTIMIZATION} {FLAGS}")
        objects.append(target)
    for source in sorted(Path(build / "../runtime").glob("*.c")):
        if source.name in left_out:
            continue
        target = f"obj/runtime_{source.stem}.o"
        lines.append(f"build {target}: runtime_cc ../runtime/{source.name}\n  cflags = {RUNTIME_OPTIMIZATION} {FLAGS} {defines}")
        objects.append(target)
    lines.append(f"build wii-recomp: link {' '.join(objects)}")
    (build / "build.ninja").write_text("\n".join(lines) + "\n")
    print(len(objects), "objects")


if __name__ == "__main__":
    main()
