#!/usr/bin/env python3
"""Проверка реальной упаковки с подставленными PE-инспектором, pacman и Inno-компилятором."""
from pathlib import Path
import json
import os
import shlex
import shutil
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
root = Path(tempfile.mkdtemp(prefix='vibechat-installer-tests-'))
case = root / 'repository with spaces'
gui = case / 'tools/chatgui'
build = gui / 'build'
commands = root / 'commands'
prefix = root / 'ucrt64'
scan_log = root / 'objdump.log'
copy_log = root / 'runtime-copy.log'
pacman_log = root / 'pacman.log'
environment = {**os.environ, 'MSYSTEM': 'UCRT64', 'MSYSTEM_PREFIX': str(prefix), 'WINDIR': str(root / 'Windows'),
               'OBJDUMP_CALLS': str(scan_log), 'RUNTIME_COPY_CALLS': str(copy_log), 'PACMAN_CALLS': str(pacman_log),
               'PACMAN_INSTALLED': str(root / 'installed-packages.txt')}


def write(path, text, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    if executable:
        path.chmod(0o755)
    return path


def run(*arguments, ok=True, extra=None):
    scan_log.write_text('')
    copy_log.write_text('')
    pacman_log.write_text('')
    result = subprocess.run(['bash', str(gui / 'package_win.sh'), *arguments],
                            env={**environment, **(extra or {})}, text=True, capture_output=True)
    with (root / 'checks.log').open('a') as log:
        log.write(f'{arguments!r}: exit={result.returncode}\n{result.stdout}{result.stderr}\n')
    if ok:
        assert result.returncode == 0, result.stdout + result.stderr
    else:
        assert result.returncode != 0, 'Unexpected packaging success'
        assert 'ERROR:' in result.stdout + result.stderr
    assert not list(build.glob('installer-stage.*')), 'Staging directory leaked'
    return result


def package():
    result = run()
    assert 'verified installer runtime:' not in result.stdout, 'Per-DLL verification spam returned'
    assert result.stdout.count('INFO: installer runtime staged:') == 1
    assert copy_log.read_text().splitlines() == ['runtime batch'], 'Expected one runtime copy process'
    assert pacman_log.read_text().splitlines() == ['-Qqo', '-Qql'], 'Expected two batched package queries'
    artifact = build / 'installer/VibeChat-1.2.3-win64-setup.exe'
    return json.loads(artifact.read_text())


try:
    for name in ('package_win.sh', 'compiler_runtime.sh', 'runtime_cache.sh', 'build.sh', 'install_deps_win.sh', 'installer/vibechat.iss'):
        target = gui / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, target)
    for name in ('LICENSE', 'LICENSING', 'THIRD_PARTY_NOTICES'):
        write(case / name, 'fixture license\n')
    for name in ('wintun-LICENSE.txt', 'wintun-api-LICENSE.txt'):
        write(case / 'lib' / name, 'Wintun notice\n')
    write(case / 'lib/wintun.dll', 'DLL Name: KERNEL32.dll\n')
    write(gui / 'rlottie_src/COPYING', 'rlottie notice\n')
    write(gui / 'rlottie_src/licenses/COPYING.MIT', 'MIT notice\n')
    write(prefix / 'share/licenses/fixture/COPYING', 'runtime notice\n')
    write(prefix / 'share/licenses/helper/COPYING', 'helper notice\n')
    write(root / 'Windows/System32/KERNEL32.dll', 'system file\n')
    write(commands / 'cygpath', '#!/bin/bash\nprintf "%s\\n" "$2"\n', executable=True)
    real_python = shutil.which('python3')
    write(commands / 'python3', '#!/bin/bash\n'
          'if [[ ${1:-} = -c && ${USE_UCRT_PYTHON:-0} != 1 ]]; then\n'
          '    grep -Fxq python "$PACMAN_INSTALLED" 2>/dev/null || exit 127\n'
          'fi\n'
          f'exec {shlex.quote(real_python)} "$@"\n', executable=True)
    write(commands / 'objdump', '''#!/bin/bash
set -eu
[[ "$1" = -p ]] || { echo 'unexpected extra PE inspection' >&2; exit 1; }
printf '%s\\n' "$2" >> "$OBJDUMP_CALLS"
if [[ $(head -1 "$2") = wrong-architecture ]]; then echo 'file format pei-i386'; else echo 'file format pei-x86-64'; fi
cat "$2"
''', executable=True)
    (prefix / 'bin').mkdir(parents=True)
    shutil.copy(commands / 'objdump', prefix / 'bin/objdump')
    write(commands / 'pacman', '''#!/bin/bash
set -eu
operation=$1; shift
printf '%s\\n' "$operation" >> "$PACMAN_CALLS"
[[ ${PACMAN_FAIL:-} != "$operation" ]] || exit 1
if [[ "$operation" = -Qq ]]; then
    if [[ -f "$PACMAN_INSTALLED" ]]; then cat "$PACMAN_INSTALLED"; fi
    exit 0
fi
if [[ "$operation" = -S ]]; then
    [[ "$1" = --needed && "$2" = --noconfirm ]]; shift 2
    printf '%s\\n' "$@" >> "$PACMAN_INSTALLED"
    exit 0
fi
if [[ "$operation" = -Q ]]; then
    for name in "$@"; do grep -Fxq "$name" "$PACMAN_INSTALLED" || exit 1; done
    exit 0
fi
[[ "$1" = -- ]]; shift
for target in "$@"; do
    if [[ "$operation" = -Qqo ]]; then
        [[ -f "$target" ]]
        if [[ "$target" = */helper.dll ]]; then echo fixture-helper; else echo fixture-runtime; fi
    else
        case "$target" in
            fixture-runtime) echo "$MSYSTEM_PREFIX/share/licenses/fixture/COPYING" ;;
            fixture-helper) echo "$MSYSTEM_PREFIX/share/licenses/helper/COPYING" ;;
            *) exit 1 ;;
        esac
    fi
done
''', executable=True)
    write(commands / 'cp', '''#!/bin/bash
set -eu
/bin/cp "$@"
if [[ "$1" = --parents ]]; then
    printf 'runtime batch\\n' >> "$RUNTIME_COPY_CALLS"
    if [[ -n ${MUTATE_COPY_SOURCE:-} ]]; then printf '\\nchanged during copy\\n' >> "$MUTATE_COPY_SOURCE"; fi
fi
''', executable=True)
    compiler = write(commands / 'Inno Setup/ISCC.exe', '''#!/usr/bin/env python3
from pathlib import Path
import json, os, sys
assert os.environ['MSYS2_ARG_CONV_EXCL'] == '*'
args = sys.argv[1:]
package = Path(next(a.removeprefix('/DPackageDir=') for a in args if a.startswith('/DPackageDir=')))
version = next(a.removeprefix('/DAppVersion=') for a in args if a.startswith('/DAppVersion='))
output = Path(next(a[2:] for a in args if a.startswith('/O')))
assert Path(args[-1]).name == 'vibechat.iss'
if os.environ.get('COMPILER_FAIL'): sys.exit(2)
if os.environ.get('COMPILER_NO_OUTPUT'): sys.exit(0)
files = sorted(str(p.relative_to(package)) for p in package.rglob('*') if p.is_file())
(output / f'VibeChat-{version}-win64-setup.exe').write_text(json.dumps({'files': files, 'arguments': args}))
''', executable=True)
    environment.update(ISCC=str(compiler), PATH=str(commands) + os.pathsep + os.environ['PATH'])
    write(build / 'chatgui_version.txt', '1.2.3\n')
    write(build / 'chatgui_qt_package.txt', 'Qt6\n')
    write(build / 'vibechat.exe', 'DLL Name: Qt6Widgets.dll\nDLL Name: Qt6Network.dll\nDLL Name: librlottie.dll\nDLL Name: libKF6SyntaxHighlighting.dll\n')
    libraries = {
        'Qt6Widgets.dll': ['Qt6Core.dll', 'Qt6Gui.dll'],
        'Qt6Core.dll': ['KERNEL32.dll', 'api-ms-win-core-test.dll'],
        'Qt6Gui.dll': ['helper.dll'], 'Qt6Network.dll': ['Qt6Core.dll'],
        'helper.dll': ['Qt6Core.dll'], 'librlottie.dll': ['KERNEL32.dll'], 'wintun.dll': ['KERNEL32.dll'],
        'libKF6SyntaxHighlighting.dll': ['Qt6Gui.dll', 'Qt6Network.dll'],
        'platforms/qwindows.dll': ['platform-helper.dll'],
        'imageformats/qjpeg.dll': ['jpeg-helper.dll'], 'imageformats/qwebp.dll': ['webp-helper.dll'],
        'platform-helper.dll': ['KERNEL32.dll'], 'jpeg-helper.dll': ['KERNEL32.dll'], 'webp-helper.dll': ['KERNEL32.dll'],
    }
    for name, imports in libraries.items():
        content = ''.join(f'DLL Name: {dll}\n' for dll in imports)
        write(build / name, content)
        if '/' in name:
            runtime = prefix / 'share/qt6/plugins' / name
        else:
            runtime = prefix / 'bin' / name
        if name != 'wintun.dll':
            write(runtime, content)
    for name in ('vibechat.cfg', 'chatgui.cfg', 'chat_data/chats.db', 'chat_data/media/private.bin',
                 'chatgui.log', 'vibechat.lock', 'test_gui.exe', 'CMakeCache.txt', 'Qt5Core.dll', 'obsolete.dll'):
        write(build / name, 'PRIVATE OR STALE FILE\n')

    run('--check')
    assert not (build / 'installer').exists(), 'Preflight created build artifacts'
    result = package()
    expected = {'vibechat.exe', *libraries, 'licenses/LICENSE', 'licenses/LICENSING', 'licenses/THIRD_PARTY_NOTICES',
                'licenses/wintun/wintun-LICENSE.txt', 'licenses/wintun/wintun-api-LICENSE.txt',
                'licenses/rlottie/COPYING', 'licenses/rlottie/licenses/COPYING.MIT',
                'licenses/runtime/fixture/COPYING', 'licenses/runtime/helper/COPYING'}
    assert set(result['files']) == expected, result['files']
    assert len(scan_log.read_text().splitlines()) == len(libraries) + 1, 'Expected one PE inspection per packaged binary'
    print('PASS: complete dependency graph, Qt plugin helpers and license notices; private files and stale DLLs excluded')
    assert 'libKF6SyntaxHighlighting.dll' in result['files']
    assert not any('Qml' in name or 'Quick' in name for name in result['files'])
    print('PASS: syntax highlighting DLL follows PE imports without QML/Quick runtime')

    result2 = package()
    assert result2['files'] == result['files']
    assert not scan_log.read_text(), 'Repeated packaging inspected unchanged PE files'
    print('PASS: repeated packaging reuses architecture and imports with no objdump calls; paths containing spaces')
    print('PASS: runtime copied in one batch, license owners/listing queried in two processes, one runtime summary')

    for operation in ('-Qqo', '-Qql'):
        failed = run(ok=False, extra={'PACMAN_FAIL': operation})
        assert 'ERROR: cannot ' in failed.stderr
    print('PASS: failed batched package queries reject incomplete license collection')

    write(build / 'vibechat.exe', (build / 'vibechat.exe').read_text() + '\nrelinked\n')
    assert package()['files'] == result['files']
    assert scan_log.read_text().splitlines() == [str(build / 'vibechat.exe')]
    print('PASS: standalone packaging inspects only a changed EXE')

    # Проверенные импорты относятся к конкретным байтам: изменение источника при копировании — ошибка.
    source_before = (build / 'helper.dll').read_text()
    failed = run(ok=False, extra={'MUTATE_COPY_SOURCE': str(build / 'helper.dll')})
    assert 'installer source changed during packaging: helper.dll' in failed.stderr
    write(build / 'helper.dll', source_before)
    print('PASS: source mutation during staging rejects the installer')

    driver = build / 'wintun.dll'
    saved_driver = driver.read_text()
    driver.unlink()
    failed = run(ok=False)
    assert 'missing runtime file:' in failed.stderr and 'wintun.dll' in failed.stderr
    write(driver, 'wrong-architecture\n')
    failed = run(ok=False)
    assert 'wintun.dll is not a Windows x64 binary' in failed.stderr
    failed = run(ok=False)
    assert 'wintun.dll is not a Windows x64 binary' in failed.stderr
    assert not scan_log.read_text(), 'Cached architecture rejection required another inspection'
    write(driver, saved_driver)
    print('PASS: dynamically loaded Wintun and its licenses are included; missing/wrong architecture DLL is rejected')

    capture = {'Qt6Multimedia.dll': ['Qt6Core.dll'], 'Qt6MultimediaWidgets.dll': ['Qt6Multimedia.dll'],
               'multimedia/ffmpegmediaplugin.dll': ['capture-helper.dll', 'MFPlat.dll'], 'capture-helper.dll': ['KERNEL32.dll']}
    write(root / 'Windows/System32/MFPlat.dll', 'system multimedia file\n')
    for name, imports in capture.items():
        content = ''.join(f'DLL Name: {dll}\n' for dll in imports)
        write(build / name, content)
        write(prefix / ('share/qt6/plugins' if '/' in name else 'bin') / name, content)
    write(build / 'chatgui_capture_enabled.txt', '1\n')
    failed = run(ok=False)
    assert 'rebuild Qt Multimedia with Unicode camera errors' in failed.stderr
    patched_qt = gui / 'qt_multimedia/build/fixture/runtime'
    write(build / 'chatgui_qt_multimedia_runtime.txt', str(patched_qt) + '\n')
    for name in ('Qt6Multimedia.dll', 'Qt6MultimediaWidgets.dll', 'multimedia/ffmpegmediaplugin.dll'):
        relative = ('plugins/' if '/' in name else 'bin/') + name
        content = (build / name).read_text() + '# patched Unicode camera errors\n'
        write(patched_qt / relative, content)
        write(build / name, content)
    write(patched_qt / 'licenses/SOURCE.txt', 'Qt Multimedia source and Unicode patch notice\n')
    captured = package()
    assert set(captured['files']) == expected | set(capture) | {'licenses/qt-multimedia/SOURCE.txt'}
    plugin = build / 'multimedia/ffmpegmediaplugin.dll'
    patched_plugin = plugin.read_text()
    write(plugin, (prefix / 'share/qt6/plugins/multimedia/ffmpegmediaplugin.dll').read_text())
    failed = run(ok=False)
    assert 'patched Qt runtime differs: multimedia/ffmpegmediaplugin.dll' in failed.stderr
    write(plugin, patched_plugin)
    (build / 'capture-helper.dll').unlink()
    failed = run(ok=False)
    assert 'ffmpegmediaplugin.dll requires capture-helper.dll' in failed.stderr
    write(build / 'capture-helper.dll', 'DLL Name: KERNEL32.dll\n')
    write(build / 'chatgui_capture_enabled.txt', '0\n')
    print('PASS: capture installer includes Multimedia plugin dependencies and rejects incomplete capture runtime')

    missing = build / 'jpeg-helper.dll'
    saved = missing.read_text()
    missing.unlink()
    failed = run(ok=False)
    assert 'qjpeg.dll requires jpeg-helper.dll' in failed.stderr
    assert str(build / 'imageformats/qjpeg.dll') not in scan_log.read_text().splitlines()
    write(missing, saved)
    print('PASS: cached plugin imports still reject a missing transitive dependency')

    write(build / 'helper.dll', 'wrong-architecture\n')
    failed = run(ok=False)
    assert 'not a Windows x64 binary' in failed.stderr
    write(build / 'helper.dll', 'DLL Name: Qt6Core.dll\n')
    print('PASS: wrong binary architecture is rejected')

    write(build / 'Qt6Core.dll', 'DLL Name: vcruntime140.dll\n')
    write(root / 'Windows/System32/vcruntime140.dll', 'developer-only redistributable\n')
    failed = run(ok=False)
    assert 'requires vcruntime140.dll' in failed.stderr
    write(build / 'Qt6Core.dll', 'DLL Name: KERNEL32.dll\n')
    print('PASS: developer-installed MSVC runtime does not masquerade as an OS dependency')

    write(build / 'helper.dll', 'DLL Name: Qt5Core.dll\n')
    failed = run(ok=False)
    assert 'mixed Qt runtime' in failed.stderr
    write(build / 'helper.dll', 'DLL Name: Qt6Core.dll\n')
    print('PASS: an imported DLL from a different Qt version is rejected')

    write(build / 'chatgui_version.txt', 'invalid\n')
    run(ok=False)
    write(build / 'chatgui_version.txt', '1.2.3\n')
    previous = (build / 'installer/VibeChat-1.2.3-win64-setup.exe').read_bytes()
    run(ok=False, extra={'COMPILER_FAIL': '1'})
    run(ok=False, extra={'COMPILER_NO_OUTPUT': '1'})
    assert (build / 'installer/VibeChat-1.2.3-win64-setup.exe').read_bytes() == previous
    print('PASS: invalid version and compiler failures cannot reuse stale output as success')

    run('--check', ok=False, extra={'ISCC': str(root / 'missing.exe')})
    run('--check', ok=False, extra={'MSYSTEM': ''})
    print('PASS: missing Inno compiler and non-Windows shell fail preflight')

    # Проверяем уведомления кастомного FFmpeg вместе со статически встроенными кодеками.
    original_exe = (build / 'vibechat.exe').read_text()
    write(build / 'vibechat.exe', original_exe + 'DLL Name: avcodec-61.dll\n')
    write(build / 'avcodec-61.dll', 'DLL Name: KERNEL32.dll\n')
    write(gui / 'ffmpeg/ffmpeg_build/install/bin/avcodec-61.dll', 'DLL Name: KERNEL32.dll\n')
    write(gui / 'ffmpeg/ffmpeg_build/src/ffmpeg/COPYING.GPLv3', 'FFmpeg license\n')
    write(gui / 'ffmpeg/ffmpeg_build/src/x264/COPYING', 'x264 license\n')
    ffmpeg = package()
    assert 'avcodec-61.dll' in ffmpeg['files']
    assert 'licenses/ffmpeg/ffmpeg/COPYING.GPLv3' in ffmpeg['files']
    assert 'licenses/ffmpeg/x264/COPYING' in ffmpeg['files']
    write(build / 'vibechat.exe', original_exe)
    shutil.rmtree(gui / 'ffmpeg')
    (build / 'avcodec-61.dll').unlink()
    print('PASS: custom FFmpeg and its static codec license notices are included')

    # Исполняем настоящий GUI build.sh: пакетирование обязано быть частью успешной Release-сборки.
    dependency_log = root / 'dependency-preparation.log'
    write(gui.parent / 'prepare_dependencies.py', '''import os, pathlib, sys
assert sys.argv[1:4] in (['--only', name, '--if-needed'] for name in ('common', 'wintun', 'rlottie')), sys.argv
with pathlib.Path(''' + repr(str(dependency_log)) + ''').open('a') as log:
    log.write(sys.argv[2] + '\\n')
if os.environ.get('FAIL_WINTUN') == '1' and sys.argv[2] == 'wintun':
    raise SystemExit('ERROR: Wintun fixture preparation failed')
''')
    write(build / 'CMakeCache.txt', 'CMAKE_GENERATOR:INTERNAL=Ninja\n')
    write(commands / 'cmake', '''#!/bin/bash
set -eu
printf '%s\\n' "$*" >> "$CMAKE_CALLS"
''', executable=True)
    shutil.copy(commands / 'objdump', prefix / 'bin/objdump')
    write(gui / 'setup_firewall.bat', 'fixture\n')
    write(gui / 'rlottie_build/librlottie.dll', 'DLL Name: KERNEL32.dll\n')
    for name in ('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll', 'zlib1.dll', 'libcrypto-3.dll'):
        write(prefix / 'bin' / name, 'DLL Name: KERNEL32.dll\n')
        original_exe += f'DLL Name: {name}\n'
    write(build / 'vibechat.exe', original_exe)
    calls = root / 'cmake-calls.log'
    ffmpeg_calls = root / 'ffmpeg-calls.log'
    write(gui / 'ffmpeg/ffmpeg_build/install/lib/pkgconfig/libavcodec.pc', 'fixture FFmpeg\n')
    write(gui / 'ffmpeg/build_ffmpeg.sh', '''#!/bin/bash
set -eu
printf 'profile update\\n' >> "$FFMPEG_CALLS"
if [[ ${FFMPEG_FAIL:-0} = 1 ]]; then exit 1; fi
''')
    environment['FFMPEG_CALLS'] = str(ffmpeg_calls)
    # Ошибка установки останавливает сборку до CMake/создания артефактов.
    failed_deps = subprocess.run(['bash', str(gui / 'build.sh'), '--installer', '-j2'], cwd=gui,
                                env={**environment, 'CMAKE_CALLS': str(calls), 'PACMAN_FAIL': '-S'}, text=True, capture_output=True)
    assert failed_deps.returncode != 0 and 'dependency installation failed' in failed_deps.stdout
    assert not calls.exists(), 'CMake ran after a failed package installation'
    failed_wintun = subprocess.run(['bash', str(gui / 'build.sh'), '--installer', '-j2'], cwd=gui,
                                  env={**environment, 'CMAKE_CALLS': str(calls), 'FAIL_WINTUN': '1'}, text=True, capture_output=True)
    assert failed_wintun.returncode != 0 and 'wintun dependency preparation failed' in failed_wintun.stdout
    assert not calls.exists(), 'CMake ran after a failed Wintun preparation'
    assert dependency_log.read_text().splitlines() == ['common', 'wintun']
    dependency_log.unlink()
    (build / 'wintun.dll').unlink()
    scan_log.write_text('')
    built = subprocess.run(['bash', str(gui / 'build.sh'), '--installer', '-j2'], cwd=gui,
                           env={**environment, 'CMAKE_CALLS': str(calls)}, text=True, capture_output=True)
    assert built.returncode == 0, built.stdout + built.stderr
    assert dependency_log.read_text().splitlines() == ['common', 'wintun', 'rlottie'], 'Windows build skipped preparation or requested Android'
    assert '-DCMAKE_BUILD_TYPE=Release' in calls.read_text()
    assert 'installer created:' in built.stdout and 'Build SUCCESS' in built.stdout
    assert 'installer runtime staged:' in built.stdout and 'scanned=0 cached=' in built.stdout, built.stdout
    assert 'verified installer runtime:' not in built.stdout
    scans = scan_log.read_text().splitlines()
    assert len(scans) == len(set(scans)), 'Packaging repeated a PE inspection performed by deployment'
    assert (build / 'wintun.dll').read_bytes() == (case / 'lib/wintun.dll').read_bytes()
    assert ffmpeg_calls.read_text().splitlines() == ['profile update']
    installed = Path(environment['PACMAN_INSTALLED']).read_text().splitlines()
    assert 'mingw-w64-ucrt-x86_64-syntax-highlighting' in installed
    assert 'mingw-w64-ucrt-x86_64-gcc' in installed and 'mingw-w64-ucrt-x86_64-openssl' in installed
    assert 'python' in installed, 'Dependency preparation requires Python in MSYS2'
    assert {'autoconf-wrapper', 'automake-wrapper'} <= set(installed), 'Clean core builds require Autotools'
    assert 'libtool' not in installed, 'This project does not use Libtool'
    assert not {'autoconf', 'automake'} & set(installed), 'Package checks must use installed provider names, not virtual aliases'
    assert 'mingw-w64-ucrt-x86_64-ffmpeg' in installed, 'Clean GUI builds require FFmpeg for video'
    assert 'mingw-w64-ucrt-x86_64-qt6-declarative' not in installed
    pacman_log.write_text('')
    deps_ready = subprocess.run(['bash', str(gui / 'install_deps_win.sh')], env=environment, text=True, capture_output=True)
    assert deps_ready.returncode == 0 and 'установка пропущена' in deps_ready.stdout
    assert pacman_log.read_text().splitlines() == ['-Qq'], 'Ready build environment tried to install packages'
    pacman_log.write_text('')
    deps_checked = subprocess.run(['bash', str(gui / 'install_deps_win.sh'), '--check'],
                                  env={**environment, 'PACMAN_FAIL': '-S'}, text=True, capture_output=True)
    assert deps_checked.returncode == 0 and pacman_log.read_text().splitlines() == ['-Qq']
    Path(environment['PACMAN_INSTALLED']).write_text('\n'.join(p for p in installed if p != 'python') + '\n')
    pacman_log.write_text('')
    alternate_python = subprocess.run(['bash', str(gui / 'install_deps_win.sh'), '--check'],
                                      env={**environment, 'PACMAN_FAIL': '-S', 'USE_UCRT_PYTHON': '1'},
                                      text=True, capture_output=True)
    assert alternate_python.returncode == 0, alternate_python.stdout + alternate_python.stderr
    assert pacman_log.read_text().splitlines() == ['-Qq'], 'Usable UCRT Python caused package installation'
    assert 'Используется доступный Python 3.9+' in alternate_python.stdout
    Path(environment['PACMAN_INSTALLED']).write_text('\n'.join(installed) + '\n')
    print('PASS: usable UCRT Python without MSYS python/libtool packages passes the offline dependency check')
    offline_build = subprocess.run(['bash', str(gui / 'build.sh'), '--installer', '-j2'], cwd=gui,
                                   env={**environment, 'CMAKE_CALLS': str(calls), 'PACMAN_FAIL': '-S'},
                                   text=True, capture_output=True)
    assert offline_build.returncode == 0, offline_build.stdout + offline_build.stderr
    print('PASS: prepared Windows GUI builds with package installation disabled; check-only uses local package queries')
    cached_ffmpeg = root / 'cached-ffmpeg'
    cached_commands = root / 'cached-codec-commands'
    write(cached_commands / 'uname', '#!/bin/sh\necho MINGW64_NT-10.0\n', executable=True)
    for name in ('pacman', 'curl', 'git', 'cmake', 'make', 'nasm', 'pkg-config', 'patch'):
        write(cached_commands / name, '#!/bin/sh\necho "Unexpected codec preparation command: $0" >&2\nexit 99\n', executable=True)
    cached_ffmpeg.mkdir()
    shutil.copyfile(source / 'ffmpeg/build_ffmpeg.sh', cached_ffmpeg / 'build_ffmpeg.sh')
    write(cached_ffmpeg / 'ffmpeg_build/install/lib/pkgconfig/libavcodec.pc', 'ready FFmpeg\n')
    write(cached_ffmpeg / 'ffmpeg_build/install/share/utun-chatgui-ffmpeg-minimal-v8', 'ready\n')
    codecs_ready = subprocess.run(['bash', str(cached_ffmpeg / 'build_ffmpeg.sh')],
                                  env={**environment, 'PATH': str(cached_commands) + os.pathsep + environment['PATH']},
                                  text=True, capture_output=True)
    assert codecs_ready.returncode == 0, codecs_ready.stdout + codecs_ready.stderr
    print('PASS: actual cached FFmpeg helper skips package checks, downloads and compiler tools offline')
    # Из готового набора пропала ровно одна библиотека: устанавливаем только её.
    syntax_package = 'mingw-w64-ucrt-x86_64-syntax-highlighting'
    Path(environment['PACMAN_INSTALLED']).write_text('\n'.join(p for p in installed if p != syntax_package) + '\n')
    before_check = Path(environment['PACMAN_INSTALLED']).read_bytes()
    pacman_log.write_text('')
    deps_checked = subprocess.run(['bash', str(gui / 'install_deps_win.sh'), '--check'],
                                  env={**environment, 'PACMAN_FAIL': '-S'}, text=True, capture_output=True)
    assert deps_checked.returncode == 3 and syntax_package in deps_checked.stdout
    assert pacman_log.read_text().splitlines() == ['-Qq'], 'Checking missing packages accessed installation or repositories'
    assert Path(environment['PACMAN_INSTALLED']).read_bytes() == before_check, 'Check-only changed installed packages'
    deps_missing = subprocess.run(['bash', str(gui / 'install_deps_win.sh')], env=environment, text=True, capture_output=True)
    assert deps_missing.returncode == 0 and 'нужно установить: 1.' in deps_missing.stdout and syntax_package in deps_missing.stdout
    assert len(Path(environment['PACMAN_INSTALLED']).read_text().splitlines()) == len(installed)
    print('PASS: missing packages installed once, existing packages untouched; install failure stops before CMake')
    print('PASS: build --installer shares cached PE format/imports without rescanning DLLs in staging')
    write(gui / 'qt_multimedia/build.sh', '''#!/bin/bash
set -eu
printf '%s\\n' "$PATCHED_QT_RUNTIME" > "$1/chatgui_qt_multimedia_runtime.txt"
''')
    write(build / 'chatgui_capture_enabled.txt', '1\n')
    write(prefix / 'bin/windeployqt6.exe', '''#!/bin/bash
set -eu
if [[ "$1" = --version ]]; then echo 'windeployqt 6.11.2'; exit; fi
destination=${@: -1}; destination=${destination%/*}
cp "$MSYSTEM_PREFIX/bin/Qt6Multimedia.dll" "$destination/Qt6Multimedia.dll"
cp "$MSYSTEM_PREFIX/share/qt6/plugins/multimedia/ffmpegmediaplugin.dll" "$destination/multimedia/ffmpegmediaplugin.dll"
''', executable=True)
    rebuilt = subprocess.run(['bash', str(gui / 'build.sh'), '--installer', '-j2'], cwd=gui,
                             env={**environment, 'CMAKE_CALLS': str(calls), 'PATCHED_QT_RUNTIME': str(patched_qt)},
                             text=True, capture_output=True)
    assert rebuilt.returncode == 0, rebuilt.stdout + rebuilt.stderr
    assert (build / 'Qt6Multimedia.dll').read_bytes() == (patched_qt / 'bin/Qt6Multimedia.dll').read_bytes()
    assert plugin.read_bytes() == (patched_qt / 'plugins/multimedia/ffmpegmediaplugin.dll').read_bytes()
    print('PASS: GUI deployment restores patched Qt DLLs after windeployqt; installer rejects the original camera plugin')
    write(build / 'chatgui_capture_enabled.txt', '0\n')
    cmake_before_failure = calls.read_text()
    codec_failed = subprocess.run(['bash', str(gui / 'build.sh'), '--installer', '-j2'], cwd=gui,
                                  env={**environment, 'CMAKE_CALLS': str(calls), 'FFMPEG_FAIL': '1'}, text=True, capture_output=True)
    assert codec_failed.returncode != 0 and 'bundled FFmpeg profile update failed' in codec_failed.stdout
    assert calls.read_text() == cmake_before_failure, 'CMake ran after a failed FFmpeg profile update'
    print('PASS: Windows GUI build updates bundled FFmpeg before CMake and stops on update failure')
    failed = subprocess.run(['bash', str(gui / 'build.sh'), '--installer'], cwd=gui,
                            env={**environment, 'CMAKE_CALLS': str(calls), 'COMPILER_FAIL': '1'}, text=True, capture_output=True)
    assert failed.returncode != 0 and 'Build SUCCESS' not in failed.stdout
    print('PASS: GUI installer flag selects Release and propagates packaging errors')

    shutil.rmtree(root)
except Exception:
    print(f'ERROR: installer fixture retained: {root}')
    raise
