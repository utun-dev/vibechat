#!/usr/bin/env python3
"""DEB или пользовательский архив под Xvfb с ALSA null; control-порт 9999 должен быть свободен."""
from pathlib import Path
import configparser
import os
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import tarfile
import time

root = Path(tempfile.mkdtemp(prefix='vibechat-clean-install-'))
stage = root / 'installed application'
audio_config = root / 'asound.conf'
audio_config.write_text('pcm.!default { type null }\n')
processes = []


def launch_text(output, profile):
    log = profile / 'chatgui.log'
    return output.read_text(errors='replace') + (log.read_text(errors='replace') if log.exists() else '')


def wait_for(predicate, message, process, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        if process.poll() is not None:
            raise AssertionError(f'{message}: application exited with {process.returncode}; logs: {root}')
        time.sleep(0.1)
    raise AssertionError(f'{message}: timed out; logs: {root}')


def read_config(path):
    config = configparser.ConfigParser(interpolation=None, strict=False)
    assert config.read(path, encoding='utf-8'), f'Config missing: {path}'
    return config


def stop(process):
    process.send_signal(signal.SIGTERM)
    assert process.wait(timeout=45) == 0, f'Unclean shutdown; logs: {root}'


def launch(name, first_run, adjacent=False):
    case = root / name
    case.mkdir()
    environment = {k: v for k, v in os.environ.items() if k not in ('LD_LIBRARY_PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH')}
    environment.update(HOME=str(case), XDG_DATA_HOME=str(case / 'data'), XDG_CACHE_HOME=str(case / 'cache'),
                       XDG_CONFIG_HOME=str(case / 'config'), XDG_RUNTIME_DIR=str(case / 'runtime'),
                       ALSA_CONFIG_PATH=str(audio_config),
                       QT_QPA_PLATFORM='xcb', QT_IM_MODULE='compose', LANG='C.UTF-8', LC_ALL='C.UTF-8')
    (case / 'runtime').mkdir(mode=0o700)
    profile = binary.parent if adjacent else case / 'data/vibechat'
    autostart = case / 'config/autostart/org.utun.vibechat.desktop'
    output = case / 'application.log'
    with output.open('w') as stream:
        process = subprocess.Popen([str(binary)], cwd=work, env=environment, stdout=stream, stderr=subprocess.STDOUT)
    processes.append(process)
    if first_run:
        wait_for(lambda: 'First-run setup: requesting nickname' in launch_text(output, profile), 'First-run wizard', process)
        windows = []
        def find_window():
            result = subprocess.run(['xdotool', 'search', '--all', '--onlyvisible', '--pid', str(process.pid), '--name', 'Welcome'],
                                    capture_output=True, text=True, env=environment)
            windows[:] = result.stdout.splitlines() if result.returncode == 0 else []
            return bool(windows)
        wait_for(find_window, 'Visible first-run window', process)
        assert not autostart.exists(), 'Autostart must wait for successful profile creation'
        subprocess.run(['xdotool', 'windowfocus', '--sync', windows[0]], check=True, env=environment)
        # Return закрывает wizard на KeyPress; KeyRelease отправляем текущему фокусу, а не уничтоженному окну.
        subprocess.run(['xdotool', 'key', 'Return'], check=True, env=environment)
    wait_for(lambda: 'GUI startup: minimized_requested=' in launch_text(output, profile), 'GUI and core ready', process)
    config = read_config(profile / 'vibechat.cfg')
    db = Path(config['gui']['db_path'])
    assert db.is_absolute() and db == profile / 'chat_data'
    assert Path(config['gui']['debug_file']) == profile / 'chatgui.log'
    assert (db / 'chats.db').is_file(), f'Database missing; logs: {root}'
    with sqlite3.connect(f'file:{db / "chats.db"}?mode=ro', uri=True) as connection:
        assert connection.execute('PRAGMA integrity_check').fetchone() == ('ok',)
    assert (db / 'dm_media').is_dir() and (db / 'dm_pending').is_dir()
    assert (db / 'member_files').is_dir()
    assert (db / 'control_server.log').is_file()
    assert list(work.iterdir()) == [], f'Application wrote into working directory; logs: {root}'
    assert autostart.exists() == first_run, 'Autostart should default to enabled only for a new profile'
    if first_run:
        startup = read_config(autostart)['Desktop Entry']
        assert startup['Hidden'] == 'false' and startup['Exec'].endswith(' --minimized')
    stop(process)
    print(f'PASS: {name}: profile, absolute paths, SQLite, media roots, clean shutdown', flush=True)
    return profile


try:
    assert os.geteuid() != 0, 'Run as an ordinary user to test write permissions'
    artifact = Path(sys.argv[1]).resolve()
    if artifact.name.endswith('.tar.gz'):
        with tarfile.open(artifact) as archive:
            archive.extractall(stage, filter='data')
        installer, = stage.glob('*/install.sh')
        environment = {**os.environ, 'HOME': str(stage), 'XDG_DATA_HOME': str(stage / 'data')}
        subprocess.run(['bash', str(installer)], env=environment, check=True)
        binary = stage / '.local/opt/vibechat/bin/vibechat'
    else:
        subprocess.run(['dpkg-deb', '-x', str(artifact), str(stage)], check=True)
        binary = stage / 'opt/vibechat/bin/vibechat'
    work = root / 'unrelated working directory'
    work.mkdir(mode=0o555)
    binary.parent.chmod(0o555)
    home_profile = launch('clean installation', True)
    original = read_config(home_profile / 'vibechat.cfg')

    # Повторный запуск сохраняет профиль и явное отключение автозапуска, мастер повторно не появляется.
    case = root / 'clean installation'
    autostart = case / 'config/autostart/org.utun.vibechat.desktop'
    autostart.unlink()
    environment = {**os.environ, 'HOME': str(case), 'XDG_DATA_HOME': str(case / 'data'),
                   'XDG_CACHE_HOME': str(case / 'cache'), 'XDG_CONFIG_HOME': str(case / 'config'),
                   'XDG_RUNTIME_DIR': str(case / 'runtime'), 'QT_QPA_PLATFORM': 'xcb', 'QT_IM_MODULE': 'compose'}
    environment['ALSA_CONFIG_PATH'] = str(audio_config)
    output = case / 'restart.log'
    with output.open('w') as stream:
        process = subprocess.Popen([str(binary), '--minimized'], cwd=work, env=environment, stdout=stream, stderr=subprocess.STDOUT)
    processes.append(process)
    wait_for(lambda: 'GUI startup: minimized_requested=1' in launch_text(output, home_profile), 'Restart', process)
    assert 'First-run setup: requesting nickname' not in launch_text(output, home_profile)
    stop(process)
    assert not autostart.exists(), 'Restart must preserve disabled autostart for an existing profile'
    assert read_config(home_profile / 'vibechat.cfg')['global']['my_private_key'] == original['global']['my_private_key']
    print('PASS: restart preserves identity, reuses the user profile and keeps autostart disabled', flush=True)

    binary.parent.chmod(0o755)
    config_path = binary.parent / 'vibechat.cfg'
    original['gui']['db_path'] = str(binary.parent / 'chat_data')
    original['gui']['debug_file'] = str(binary.parent / 'chatgui.log')
    with config_path.open('w') as stream:
        original.write(stream)
    launch('adjacent profile', False, adjacent=True)
    preserved = config_path.read_bytes()
    config_path.chmod(0o444)
    launch('read-only adjacent config', True)
    assert config_path.read_bytes() == preserved
    config_path.chmod(0o644)
    binary.parent.chmod(0o555)
    launch('read-only binary directory', True)
    assert config_path.read_bytes() == preserved
except BaseException:
    print(f'Clean-install startup test failed; logs retained: {root}', flush=True)
    raise
else:
    print(f'PASS: Linux package startup scenarios completed; logs: {root}', flush=True)
finally:
    for process in processes:
        if process.poll() is None:
            process.kill()
            process.wait()
    if 'binary' in globals() and binary.parent.exists():
        binary.parent.chmod(0o755)
    if 'work' in globals():
        work.chmod(0o755)
