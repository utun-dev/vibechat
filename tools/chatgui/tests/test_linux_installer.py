#!/usr/bin/env python3
"""Настоящий DEB из маленького ELF-проекта: зависимости, плагины, лицензии и переносимость runtime."""
from pathlib import Path
import base64
import os
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import xml.etree.ElementTree as ET

source = Path(__file__).resolve().parents[1]
root = Path(tempfile.mkdtemp(prefix='vibechat-deb-tests-'))
gui = root / 'repository with spaces/tools/chatgui'
runtime = gui / 'runtime'
plugins = gui / 'qt plugins'
environment = {k: v for k, v in os.environ.items() if k not in ('MSYSTEM', 'LD_LIBRARY_PATH', 'DESTDIR')}


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def run(*args, ok=True):
    result = subprocess.run(args, env=environment, capture_output=True, text=True)
    with (root / 'checks.log').open('a') as log:
        log.write(f'{args!r}: exit={result.returncode}\n{result.stdout}{result.stderr}\n')
    if ok:
        assert result.returncode == 0, result.stdout + result.stderr
    else:
        assert result.returncode != 0, 'Unexpected success'
        assert 'ERROR:' in result.stdout + result.stderr or 'CMake Error' in result.stderr
    return result


try:
    for relative in ('build_nix.sh', 'package_deb.sh', 'installer/linux.cmake', 'installer/deploy_linux.cmake.in',
                     'installer/linux_dependencies.cmake.in',
                     'installer/linux_licenses.py', 'installer/org.utun.vibechat.desktop',
                     'installer/org.utun.vibechat.appstream.xml'):
        target = gui / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / relative, target)
    (gui / 'fixture-icon.png').write_bytes(base64.b64decode(
        'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jBxkAAAAASUVORK5CYII='))
    for name in ('LICENSE', 'LICENSING', 'THIRD_PARTY_NOTICES'):
        write(gui.parents[1] / name, 'fixture project license\n')
    dependency_log = root / 'dependency-preparation.log'
    write(gui.parent / 'prepare_dependencies.py', '''import pathlib, sys
assert sys.argv[1:4] in (['--only', 'common', '--if-needed'], ['--only', 'rlottie', '--if-needed']), sys.argv
with pathlib.Path(''' + repr(str(dependency_log)) + ''').open('a') as log:
    log.write(sys.argv[2] + '\\n')
''')
    # Локальная rlottie хранит уведомления рядом с исходниками, отдельно от каталога ELF-сборки.
    rlottie = write(gui / 'rlottie_build/librlottie.so.0.2', 'local runtime fixture\n')
    write(gui / 'rlottie_src/COPYING', 'rlottie LGPL notice\n')
    write(gui / 'rlottie_src/licenses/COPYING.MIT', 'bundled MIT notice\n')
    local_manifest = write(root / 'rlottie-runtime.txt', str(rlottie) + '\n')
    run('python3', str(gui / 'installer/linux_licenses.py'), str(local_manifest), str(root / 'rlottie-notices'))
    assert (root / 'rlottie-notices/rlottie/rlottie_src/COPYING').read_text() == 'rlottie LGPL notice\n'
    assert (root / 'rlottie-notices/rlottie/licenses/COPYING.MIT').read_text() == 'bundled MIT notice\n'
    print('PASS: locally built rlottie includes its main and bundled component license notices', flush=True)
    write(runtime / 'LICENSE', 'fixture private runtime license\n')
    write(plugins / 'LICENSE', 'fixture plugin license\n')
    write(gui / 'helper.c', 'int helper(void) { return 20; }\n')
    write(gui / 'fixture.c', 'int helper(void); int fixture(void) { return 22 + helper(); }\n')
    write(gui / 'pluginhelper.c', 'int pluginhelper(void) { return 5; }\n')
    write(gui / 'plugin.c', 'int pluginhelper(void); int plugin_result(void) { return pluginhelper(); }\n')
    for name, file in (('helper', 'helper.c'), ('fixture', 'fixture.c'), ('pluginhelper', 'pluginhelper.c')):
        arguments = ['cc', '-shared', '-fPIC', str(gui / file), '-Wl,-soname,lib' + name + '.so.1']
        if name == 'fixture':
            arguments += ['-L' + str(runtime), '-lhelper', '-Wl,-rpath,' + str(runtime)]
        run(*arguments, '-o', str(runtime / f'lib{name}.so.1'))
        (runtime / f'lib{name}.so').symlink_to(f'lib{name}.so.1')
    required = ('platforms/libqxcb.so', 'platforms/libqoffscreen.so', 'imageformats/libqjpeg.so',
                'imageformats/libqwebp.so', 'tls/libqopensslbackend.so', 'multimedia/libffmpegmediaplugin.so')
    for relative in required:
        target = plugins / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        run('cc', '-shared', '-fPIC', str(gui / 'plugin.c'), '-L' + str(runtime), '-lpluginhelper',
            '-Wl,-rpath,' + str(runtime), '-o', str(target))
    qmake = write(gui / 'qmake', '#!/bin/sh\nprintf "%s\\n" ' + shlex.quote(str(plugins)) + '\n')
    qmake.chmod(0o755)
    write(gui / 'main.c', '''#include <stdio.h>
#include <dlfcn.h>
#include <zlib.h>
int fixture(void);
int main(int argc, char **argv) {
    if (!zlibVersion()) return 3;
    if (argc > 1) {
        void *plugin = dlopen(argv[1], RTLD_NOW);
        if (!plugin) { fprintf(stderr, "%s\\n", dlerror()); return 1; }
        int (*result)(void) = dlsym(plugin, "plugin_result");
        if (!result) return 2;
        printf("%d\\n", result()); dlclose(plugin);
    } else printf("%d\\n", fixture());
    return 0;
}
''')
    write(gui / 'CMakeLists.txt', '''cmake_minimum_required(VERSION 3.21)
project(vibechat VERSION 1.2.3 LANGUAGES C)
file(WRITE "${CMAKE_BINARY_DIR}/chatgui_version.txt" "${PROJECT_VERSION}\\n")
option(CHATGUI_BUILD_DEB "Prepare the DEB" OFF)
add_executable(vibechat main.c)
find_package(ZLIB REQUIRED)
target_link_libraries(vibechat PRIVATE "${CMAKE_SOURCE_DIR}/runtime/libfixture.so.1" dl ZLIB::ZLIB)
set(QT_PACKAGE Qt6)
set(GUI_CAPTURE_ENABLED 1)
set(QT_QMAKE "${CMAKE_SOURCE_DIR}/qmake")
set(CHATGUI_ICON_PNG "${CMAKE_CURRENT_BINARY_DIR}/app_icon.png")
configure_file(fixture-icon.png "${CHATGUI_ICON_PNG}" COPYONLY)
set(UTUN_LICENSE_FILES "${CMAKE_SOURCE_DIR}/../../LICENSE" "${CMAKE_SOURCE_DIR}/../../LICENSING"
    "${CMAKE_SOURCE_DIR}/../../THIRD_PARTY_NOTICES")
include(installer/linux.cmake)
''')
    for args in (('--deb', '--asan'), ('--deb', '-j0'), ('--invalid',), ('--deb', '-j')):
        run('bash', str(gui / 'build_nix.sh'), *args, ok=False)
    run('bash', str(gui / 'package_deb.sh'), '--check')
    assert not (gui / 'build-deb').exists(), 'Preflight created build artifacts'
    run('bash', str(gui / 'build_nix.sh'), '--deb', '-j', '2')
    assert dependency_log.read_text().splitlines() == ['common', 'rlottie'], 'Desktop build skipped preparation or requested Android'
    artifact, = (gui / 'build-deb/installer').glob('vibechat_1.2.3_*.deb')
    assert run('dpkg-deb', '-f', str(artifact), 'Package').stdout.strip() == 'vibechat'
    dependencies = run('dpkg-deb', '-f', str(artifact), 'Depends').stdout
    assert 'libc6' in dependencies and 'libasound2' in dependencies
    assert 'zlib1g' not in dependencies, 'Bundled system library must not also become an APT dependency'
    archive = root / 'payload.tar'
    with archive.open('wb') as stream:
        subprocess.run(['dpkg-deb', '--fsys-tarfile', str(artifact)], stdout=stream, check=True, env=environment)
    with tarfile.open(archive) as payload:
        assert all(member.uid == member.gid == 0 for member in payload), 'DEB files must be owned by root'
    write(gui / 'build-deb/vibechat.cfg', 'PRIVATE CONFIG\n')
    write(gui / 'build-deb/chat_data/chats.db', 'PRIVATE DATABASE\n')
    main_source = gui / 'main.c'
    changed_main = main_source.read_text().replace('} else printf("%d\\n", fixture());', '} else printf("%d\\n", fixture() + 1);')
    assert 'fixture() + 1' in changed_main
    write(main_source, changed_main)
    # Прямая упаковка обязана пересобрать изменённый исходник, а не взять прежний ELF.
    run('bash', str(gui / 'package_deb.sh'))
    stage = root / 'extracted'
    run('dpkg-deb', '-x', str(artifact), str(stage))
    app = stage / 'opt/vibechat'
    assert (stage / 'usr/bin/vibechat').resolve() == app / 'bin/vibechat'
    assert (stage / 'usr/share/applications/org.utun.vibechat.desktop').exists()
    assert (stage / 'usr/share/pixmaps/org.utun.vibechat.png').exists()
    catalog = stage / 'usr/share/swcatalog/xml/org.utun.vibechat.appstream.xml'
    run('appstreamcli', 'validate', '--no-net', str(catalog))
    parsed_catalog = root / 'parsed-appstream.xml'
    run('appstreamcli', 'convert', '--format=xml', str(catalog), str(parsed_catalog))
    data = ET.parse(parsed_catalog).getroot().find('component')
    assert data.findtext('id') == 'org.utun.vibechat'
    assert data.findtext('pkgname') == 'vibechat', 'Discover cannot show a component without a package name'
    assert data.findtext('launchable') == 'org.utun.vibechat.desktop'
    assert (stage / data.findtext('icon').lstrip('/')).is_file()
    print('PASS: AppStream parses the packaged catalog with package vibechat, its launcher and bundled icon', flush=True)
    assert not list(stage.rglob('*.cfg')) and not list(stage.rglob('*.db'))
    assert not list(stage.rglob('*.c')) and not list(stage.rglob('CMakeCache.txt'))
    assert not (stage / 'debian').exists() and not (stage / 'DEBIAN').exists()
    assert (stage / 'usr/share/doc/vibechat/copyright').is_file()
    assert (stage / 'usr/share/doc/vibechat/runtime').is_dir()
    assert list((stage / 'usr/share/doc/vibechat/runtime').rglob('LICENSE'))
    assert '$ORIGIN/../lib' in run('readelf', '-d', str(app / 'bin/vibechat')).stdout
    runtime.rename(gui / 'hidden-runtime')
    assert run(str(app / 'bin/vibechat')).stdout.strip() == '43', 'Direct packaging used a stale executable'
    assert run(str(app / 'bin/vibechat'), str(app / 'plugins/platforms/libqxcb.so')).stdout.strip() == '5'
    (gui / 'hidden-runtime').rename(runtime)
    print('PASS: real DEB rebuilds changed sources, transitive ELF/plugins, relocated runtime, licenses, no private files', flush=True)

    plugin = plugins / 'imageformats/libqwebp.so'
    saved_plugin = plugin.read_bytes()
    plugin.unlink()
    failed = run('bash', str(gui / 'package_deb.sh'), ok=False)
    assert 'Required Qt plugin missing' in failed.stderr
    plugin.write_bytes(saved_plugin)
    (runtime / 'libhelper.so.1').rename(runtime / 'missing-helper')
    failed = run('bash', str(gui / 'package_deb.sh'), ok=False)
    assert 'libhelper.so.1' in failed.stderr
    (runtime / 'missing-helper').rename(runtime / 'libhelper.so.1')
    (runtime / 'LICENSE').unlink()
    failed = run('bash', str(gui / 'package_deb.sh'), ok=False)
    assert 'no local license notices' in failed.stderr
    print('PASS: missing plugin, transitive dependency or license rejects packaging; invalid build arguments reject early', flush=True)
    write(runtime / 'LICENSE', 'fixture private runtime license\n')
    write(gui / 'build/vibechat.cfg', 'DEVELOPER PROFILE\n')
    commands = root / 'commands'
    sudo = write(commands / 'sudo', '#!/bin/sh\nprintf "%s\\n" "$*" >> ' + shlex.quote(str(root / 'sudo.log')) + '\n')
    sudo.chmod(0o755)
    environment['PATH'] = str(commands) + os.pathsep + environment['PATH']
    run('bash', str(gui / 'build_nix.sh'), '--full', '--installer', '-j2')
    dependency_commands = (root / 'sudo.log').read_text()
    assert 'apt-get update' in dependency_commands and 'apt-get install -y' in dependency_commands
    assert all(name in dependency_commands for name in ('dpkg-dev', 'qt6-multimedia-dev', 'libpipewire-0.3-dev', 'python3', 'appstream'))
    assert (gui / 'build/vibechat.cfg').read_text() == 'DEVELOPER PROFILE\n'
    print('PASS: --full installs build dependencies through APT (mock sudo); --installer alias preserves developer build/profile', flush=True)
except BaseException:
    print(f'Linux installer test failed; diagnostic files: {root}', flush=True)
    raise
else:
    shutil.rmtree(root)
