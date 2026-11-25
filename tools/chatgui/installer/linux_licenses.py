#!/usr/bin/env python3
"""Собирает copyright владельцев скопированных ELF; для локальных сборок требует исходные уведомления."""
from pathlib import Path
import shutil
import subprocess
import sys


def collect(manifest, destination):
    sources = sorted({str(Path(line).resolve()) for line in manifest.read_text().splitlines() if line})
    result = subprocess.run(['dpkg-query', '-S', *sources], text=True, capture_output=True)
    if result.returncode not in (0, 1):
        raise RuntimeError(f'dpkg-query failed: {result.stderr}')
    owners = {}
    for line in result.stdout.splitlines():
        owner, separator, path = line.partition(': ')
        if separator and path in sources:
            owners[path] = owner.split(', ')[0].split(':')[0]
    destination.mkdir(parents=True, exist_ok=True)
    for package in sorted(set(owners.values())):
        copyright_file = Path('/usr/share/doc') / package / 'copyright'
        if not copyright_file.is_file():
            raise RuntimeError(f'runtime license missing: {copyright_file}')
        target = destination / package
        target.mkdir(exist_ok=True)
        shutil.copyfile(copyright_file, target / 'copyright')

    local = 0
    gui = Path(__file__).resolve().parents[1]
    ffmpeg_install = gui / 'ffmpeg/ffmpeg_build/install'
    for path in sources:
        if path in owners:
            continue
        source = Path(path)
        if source.is_relative_to(ffmpeg_install):
            roots = sorted((gui / 'ffmpeg/ffmpeg_build/src').glob('*'))
            label = 'ffmpeg'
        elif source.is_relative_to(gui / 'rlottie_build'):
            roots = [gui / 'rlottie_src', gui / 'rlottie_src/licenses']
            label = 'rlottie'
        else:
            # Бинарные дистрибутивы, например ONNX Runtime, содержат LICENSE рядом с lib/.
            roots = [source.parent, source.parent.parent]
            label = source.name
        notices = []
        for root in roots:
            for pattern in ('COPYING*', 'LICENSE*', '*[Nn]otice*', 'PATENTS'):
                notices.extend(p for p in root.glob(pattern) if p.is_file())
        if not notices:
            raise RuntimeError(f'no package owns {source} and no local license notices were found')
        for notice in sorted(set(notices)):
            target = destination / label / notice.parent.name
            target.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(notice, target / notice.name)
        local += 1
    print(f'INFO: DEB runtime licenses: Debian packages={len(set(owners.values()))} local libraries={local}', flush=True)


if __name__ == '__main__':
    try:
        collect(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, RuntimeError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(1)
