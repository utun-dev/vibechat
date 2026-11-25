#!/usr/bin/env python3
"""Генерирует портретные KEY-пробы звонка через FFmpeg libx264/libx265."""
import pathlib
import re
import subprocess
import argparse

root = pathlib.Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--fullhd", action="store_true", help="1920x1080 and 1080x1920, level 4.0")
parser.add_argument("--fast", action="store_true", help="720p/1080p, both orientations, 60 fps")
parser.add_argument("--stdout", action="store_true", help="Print header instead of writing it")
args = parser.parse_args()
arrays = []
for width, height, codec in ([(w, h, c) for w, h in ((1280, 720), (720, 1280), (1920, 1080), (1080, 1920)) for c in (1, 2)] if args.fast
                             else [(w, h, c) for w, h in ((1920, 1080), (1080, 1920)) for c in (1, 2)] if args.fullhd
                             else [(720, 1280, c) for c in (1, 2)]):
    level = (("4.2" if codec == 1 else "4.1") if max(width, height) == 1920 else ("3.2" if codec == 1 else "4.0")) if args.fast else ("4.0" if args.fullhd else "3.1")
    fps = 60 if args.fast else 30
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", f"color=gray:size={width}x{height}:rate={fps}",
               "-frames:v", "1", "-c:v", "libx264" if codec == 1 else "libx265", "-preset", "ultrafast", "-tune", "zerolatency"]
    if codec == 1:
        command += ["-profile:v", "baseline", "-level:v", level, "-x264-params", "ref=3:slices=1:threads=1"]
    else:
        command += ["-profile:v", "main", "-x265-params", f"level-idc={level}:high-tier=0:ref=3:bframes=0:pools=none:frame-threads=1:log-level=error"]
    stream = subprocess.check_output(command + ["-f", "h264" if codec == 1 else "hevc", "pipe:1"])
    config, picture = bytearray(), bytearray()
    for nal in re.split(b"\x00\x00\x00?\x01", stream):
        if not nal:
            continue
        kind = nal[0] & 31 if codec == 1 else (nal[0] >> 1) & 63
        if kind in ((7, 8) if codec == 1 else (32, 33, 34)):
            config += b"\x00\x00\x00\x01" + nal
        elif kind in ((5,) if codec == 1 else (19, 20)):
            picture += b"\x00\x00\x00\x01" + nal
    assert config and picture
    packet = len(config).to_bytes(4, "big") + config + picture
    assert len(packet) <= 8192
    rows = ["    " + ", ".join(f"0x{byte:02x}" for byte in packet[i:i + 20]) + "," for i in range(0, len(packet), 20)]
    name = f"call_video_fast_{width}_{height}_{codec}" if args.fast else (f"call_video_fullhd_{width}_{height}_{codec}" if args.fullhd else f"call_video_portrait_{codec}")
    arrays.append(f"static const uint8_t {name}[] = {{\n" + "\n".join(rows) + "\n};\n")
kind = "fast" if args.fast else ("fullhd" if args.fullhd else "portrait")
guard = f"CALL_VIDEO_{kind.upper()}_FIXTURES_H"
header = (f"/* {kind}, {60 if args.fast else 30} fps, gray YUV420, ref=3, no B: tools/generate_call_video_portrait.py"
          + (" --fast" if args.fast else (" --fullhd" if args.fullhd else "")) + ".\n * Configuration + IDR; SEI исключены. */\n"
          + f"#ifndef {guard}\n#define {guard}\n#include <stdint.h>\n" + "".join(arrays) + "#endif\n")
if args.stdout:
    print(header, end="")
else:
    target = root / f"src/call/call_video_{kind}_fixtures.h"
    target.write_text(header)
    print(target)
