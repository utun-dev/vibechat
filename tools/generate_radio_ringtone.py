#!/usr/bin/env python3
"""Создаёт общий рингтон рации для desktop и Android; нужен ffmpeg, без сторонних Python-модулей."""

import array
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import wave


def main():
    root = Path(__file__).resolve().parents[1]
    rate = 48000
    duration = 2.4
    samples = array.array("h", [0]) * round(rate * duration)
    # Три восходящих позывных с мягкими фронтами, затем пауза до следующего цикла.
    for start, frequency in ((0.1, 660), (0.34, 880), (0.58, 1100)):
        length = 0.16
        for i in range(round(rate * length)):
            t = i / rate
            envelope = min(1.0, t / 0.008, (length - t) / 0.035)
            phase = 2 * math.pi * frequency * t
            signal = (math.sin(phase) + 0.15 * math.sin(2 * phase)) / 1.15
            samples[round(start * rate) + i] = round(32767 * 0.42 * envelope * signal)
    peak = max(abs(sample) for sample in samples) / 32767
    if sys.byteorder != "little":
        samples.byteswap()
    destinations = (
        root / "tools/chatgui/resources/sounds/radio_incoming.mp3",
        root / "tools/chatgui-android/app/src/main/res/raw/radio_incoming.mp3",
    )
    with tempfile.TemporaryDirectory(prefix="utun-radio-ring-") as directory:
        source = Path(directory) / "radio.wav"
        encoded = Path(directory) / "radio.mp3"
        with wave.open(str(source), "wb") as output:
            output.setparams((1, 2, rate, 0, "NONE", "not compressed"))
            output.writeframes(samples.tobytes())
        subprocess.run(
            ["ffmpeg", "-v", "error", "-y", "-i", str(source), "-codec:a", "libmp3lame", "-b:a", "96k", str(encoded)],
            check=True,
        )
        for destination in destinations:
            shutil.copyfile(encoded, destination)
            print(destination.relative_to(root))
    print(f"Radio ringtone: {duration}s, mono {rate}Hz, peak {20 * math.log10(peak):.1f} dBFS")


if __name__ == "__main__":
    main()
