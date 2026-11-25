#!/usr/bin/env python3
"""Изолированные установки/обновления: реальный ELF, ошибки зависимостей и откат публикации."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import platform

release = platform.freedesktop_os_release()
if release.get('VERSION_ID') != '26.04' or release.get('ID') != 'ubuntu' or platform.machine() != 'x86_64':
    print('SKIP: user installer integration requires Ubuntu 26.04')
    raise SystemExit(0)

source = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='vibechat-user-tests-') as temporary:
    root = Path(temporary)
    archive = root / 'archive with spaces'
    shutil.copytree(source / 'installer/user', archive)
    (archive / 'bin').mkdir()
    shutil.copy2('/bin/true', archive / 'bin/vibechat')
    (archive / 'licenses').mkdir()
    (archive / 'licenses/LICENSE').write_text('fixture\n')
    (archive / 'icon.png').write_bytes(b'fixture')
    (archive / '.vibechat-user-install').write_text('1\n')
    (archive / 'dependencies.txt').write_text('libc6 (>= 2.1)\n')
    (archive / 'plugins.txt').write_text('')
    home = root / 'home with spaces % and $'
    data = home / 'my data'
    profile = data / 'vibechat/profile'
    profile.parent.mkdir(parents=True)
    profile.write_text('keep my identity')
    commands = root / 'commands'
    commands.mkdir()
    environment = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(home / '.config'),
                       PATH=str(commands) + ':' + os.environ['PATH'])
    environment.pop('LD_LIBRARY_PATH', None)
    environment.pop('LD_PRELOAD', None)
    app = home / '.local/opt/vibechat'
    desktop = data / 'applications/org.utun.vibechat.desktop'

    def run(script='install.sh', *args, ok=True):
        result = subprocess.run(['bash', str(archive / script), *args], env=environment, capture_output=True, text=True)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        if not ok:
            assert 'ERROR:' in result.stdout + result.stderr
        return result.stdout + result.stderr

    def mock(name, body):
        path = commands / name
        path.write_text('#!/bin/bash\n' + body + '\n')
        path.chmod(0o755)
        return path

    for args in (('--user-archive', '--asan'), ('--user-archive', '--deb'), ('--user-archive', '-j0')):
        result = subprocess.run(['bash', str(source / 'build_nix.sh'), *args], env=environment, capture_output=True, text=True)
        assert result.returncode and 'ERROR:' in result.stderr, result.stdout + result.stderr
    run('install.sh', '--check')
    assert not app.exists() and not desktop.exists()
    run()
    assert (app / 'bin/vibechat').is_file() and desktop.exists()
    subprocess.run(['desktop-file-validate', str(desktop)], check=True)
    subprocess.run(['gio', 'launch', str(desktop)], env=environment, check=True, capture_output=True)
    assert '%%' in desktop.read_text() and '\\$' in desktop.read_text()
    assert profile.read_text() == 'keep my identity'
    (app / 'old-release').write_text('old')
    (archive / 'bin/new-release').write_text('new')

    (archive / 'dependencies.txt').write_text('vibechat-nonexistent-fixture (>= 1)\nlibc6 (>= 999)\n')
    diagnostic = run(ok=False)
    assert 'sudo apt update && sudo apt install' in diagnostic and 'libc6' in diagnostic
    assert (app / 'old-release').exists() and not (app / 'bin/new-release').exists()
    (archive / 'dependencies.txt').write_text('vibechat-nonexistent-fixture | libc6 (>= 2.1)\n')
    (archive / 'plugins.txt').write_text('/usr/lib/vibechat-missing-plugin.so\n')
    assert 'Qt plugin missing' in run(ok=False)
    (archive / 'plugins.txt').write_text('')
    ldd = mock('ldd', 'echo "libfixture.so => not found"; exit 0')
    assert 'Runtime is broken' in run(ok=False)
    ldd.unlink()
    uname = mock('uname', 'echo aarch64')
    assert 'x64 is required' in run(ok=False)
    uname.unlink()
    environment['BASH_FUNC_source%%'] = '() { ID=ubuntu; VERSION_ID=24.04; }'
    assert 'incompatible OS' in run(ok=False)
    environment.pop('BASH_FUNC_source%%')
    old_launcher = desktop.read_bytes()
    mv = mock('mv', 'if [[ "${!#}" = */org.utun.vibechat.desktop ]]; then exit 9; fi\nexec /usr/bin/mv "$@"')
    assert 'restoring previous application' in run(ok=False)
    assert (app / 'old-release').exists() and desktop.read_bytes() == old_launcher
    mv.unlink()
    run()
    assert not (app / 'old-release').exists() and (app / 'bin/new-release').exists()
    assert not list(app.parent.glob('.vibechat-*'))
    assert profile.read_text() == 'keep my identity'
    autostart = home / '.config/autostart/org.utun.vibechat.desktop'
    autostart.parent.mkdir(parents=True)
    autostart.write_text('[Desktop Entry]\n' + (app / 'autostart-exec.txt').read_text())
    run(str(app / 'uninstall.sh'))
    assert not app.exists() and not desktop.exists() and profile.exists() and not autostart.exists()
    run('uninstall.sh', ok=False)
    app.mkdir()
    assert 'unmanaged directory' in run(ok=False)
    app.rmdir()
    app.symlink_to(archive, target_is_directory=True)
    assert 'must not be a symlink' in run(ok=False)
    app.unlink()
    bad_home = root / 'home-is-file'
    bad_home.write_text('fixture')
    environment['HOME'] = str(bad_home)
    assert 'installation failed' in run(ok=False)
print('PASS: dependency versions/alternatives, missing ELF/plugins, architecture, install/update/rollback, spaces, uninstall/profile, invalid paths')
