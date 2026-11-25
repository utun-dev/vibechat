#!/usr/bin/env python3
"""Архив Ubuntu 26.04: системный runtime, зависимости ELF/Qt и пользовательский установщик."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

GUI = Path(__file__).resolve().parents[1]
ENV = {k: v for k, v in os.environ.items() if k not in ('LD_LIBRARY_PATH', 'LD_PRELOAD')}


def run(*args, cwd=None):
    result = subprocess.run(args, cwd=cwd, env=ENV, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{args!r}: {result.stdout}{result.stderr}')
    return result.stdout.strip()


def package(build):
    binary = build / 'vibechat'
    qmake = (build / 'user-qmake.txt').read_text().strip()
    plugins_dir = Path(run(qmake, '-query', 'QT_INSTALL_PLUGINS'))
    required = ('platforms/libqxcb.so', 'platforms/libqoffscreen.so', 'imageformats/libqjpeg.so',
                'imageformats/libqwebp.so', 'tls/libqopensslbackend.so', 'multimedia/libffmpegmediaplugin.so')
    plugins = [plugins_dir / name for name in required]
    objects = [binary, *plugins]
    system_files = {plugin.resolve() for plugin in plugins}
    for obj in objects:
        if not obj.is_file():
            raise RuntimeError(f'Required runtime file missing: {obj}')
        dynamic = run('readelf', '-d', str(obj))
        if obj == binary and re.search(r'\((?:RPATH|RUNPATH)\)', dynamic):
            raise RuntimeError(f'Application contains a runtime search path: {dynamic}')
        dependencies = run('ldd', str(obj))
        if 'not found' in dependencies:
            raise RuntimeError(f'Incomplete runtime for {obj}: {dependencies}')
        for name in re.findall(r'(?:=>\s+|^\s*)(/\S+)', dependencies, re.M):
            path = Path(name).resolve()
            if not path.is_relative_to('/usr/lib'):
                raise RuntimeError(f'Non-system runtime for {obj}: {path}')
            system_files.add(path)
        print(f'INFO: system ELF verified: {obj}', flush=True)
    for plugin in plugins:
        if not plugin.resolve().is_relative_to('/usr/lib'):
            raise RuntimeError(f'Non-system Qt plugin: {plugin}')
    # Один запрос вместо отдельного сканирования базы dpkg для каждой транзитивной библиотеки.
    owners = {}
    for line in run('dpkg-query', '-S', *(str(path) for path in sorted(system_files))).splitlines():
        pkg, separator, filename = line.rpartition(': ')
        if separator:
            owners[Path(filename)] = pkg
    for path in system_files:
        if path not in owners:
            raise RuntimeError(f'System runtime has no package owner: {path}')
    with tempfile.TemporaryDirectory(prefix='vibechat-user-') as temporary:
        work = Path(temporary)
        (work / 'debian').mkdir()
        (work / 'debian/control').write_text('Source: vibechat\n\nPackage: vibechat\nArchitecture: amd64\nDescription: VibeChat\n')
        # Приватные Qt ABI не имеют shlibs для сторонних пакетов. Для плагинов проверяем
        # их пакет и версию ниже; согласованность Qt обеспечивают зависимости этого пакета.
        result = run('dpkg-shlibdeps', '--warnings=1', '-O', str(binary), cwd=work)
        if not result.startswith('shlibs:Depends='):
            raise RuntimeError(f'Invalid dependency result: {result}')
        dependencies = result.removeprefix('shlibs:Depends=').split(', ')
        # dlopen-зависимости не видны в DT_NEEDED приложения.
        dependencies += ['ca-certificates', 'libasound2t64', 'libpulse0', 'fuse3']
        for plugin in plugins:
            pkg = owners[plugin.resolve()]
            version = run('dpkg-query', '-W', '-f=${Version}', pkg)
            dependencies.append(f'{pkg} (>= {version})')
        version = (build / 'chatgui_version.txt').read_text().strip()
        if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)+', version):
            raise RuntimeError(f'Invalid application version: {version}')
        name = f'vibechat-{version}-ubuntu-26.04-amd64'
        payload = work / name
        (payload / 'bin').mkdir(parents=True)
        shutil.copy2(binary, payload / 'bin/vibechat')
        run('strip', str(payload / 'bin/vibechat'))
        shutil.copy2(build / 'app_icon.png', payload / 'icon.png')
        for filename in ('install.sh', 'check.sh', 'uninstall.sh'):
            shutil.copy2(GUI / 'installer/user' / filename, payload / filename)
            (payload / filename).chmod(0o755)
        (payload / 'dependencies.txt').write_text('\n'.join(sorted(set(dependencies))) + '\n')
        (payload / 'plugins.txt').write_text('\n'.join(str(plugin) for plugin in plugins) + '\n')
        (payload / '.vibechat-user-install').write_text('1\n')
        (payload / 'licenses').mkdir()
        for filename in ('LICENSE', 'LICENSING', 'THIRD_PARTY_NOTICES'):
            shutil.copy2(GUI.parents[1] / filename, payload / 'licenses' / filename)
        shutil.copy2(GUI / 'installer/user/README.txt', payload / 'README.txt')
        destination = build / 'installer'
        destination.mkdir(exist_ok=True)
        archive = destination / f'{name}.tar.gz'
        with tarfile.open(str(archive) + '.tmp', 'w:gz') as output:
            output.add(payload, arcname=name)
        Path(str(archive) + '.tmp').replace(archive)
        print(f'INFO: system runtime verified: ELF={len(objects)} dependencies={len(set(dependencies))}')
        print(f'INFO: user archive ready: {archive}')


if __name__ == '__main__':
    try:
        package(Path(sys.argv[1]).resolve())
    except (OSError, RuntimeError, IndexError) as error:
        sys.exit(f'ERROR: user archive packaging failed: {error}')
