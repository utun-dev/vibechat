#!/usr/bin/env python3
"""Проверка настоящего build.sh на маленьком проекте Autotools, без изменения исходников uTun."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

script = Path(sys.argv[1]).resolve()
base = Path(tempfile.mkdtemp(prefix='utun-build-tests-'))
log = base / 'commands.log'
environment = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
environment.pop('MAKEFLAGS', None)
environment.pop('MSYSTEM', None)
for name in ['CC', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'DEBUG_FLAGS', 'OPENSSL_ROOT_DIR', 'CMAKE_PREFIX_PATH']:
    environment.pop(name, None)
checks = []
default_compiler = 'clang' if sys.platform.startswith('freebsd') else 'gcc'
make_tool = environment.get('MAKE') or ('gmake' if sys.platform.startswith('freebsd') else 'make')

def write(name, content):
    path = base / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    return path

def run(args, ok=True, extra_environment=None):
    if args[0] == 'make':
        args = [make_tool, *args[1:]]
    result = subprocess.run(args, cwd=base, env={**environment, **(extra_environment or {})}, text=True, capture_output=True)
    with log.open('a') as output:
        output.write(f'{args!r}\nexit={result.returncode}\n{result.stdout}{result.stderr}\n')
    if ok and result.returncode:
        raise AssertionError(f'{args!r}: exit={result.returncode}; see {log}\n{result.stderr[-1500:]}')
    return result

def build(*args, **kwargs):
    return run(['bash', 'build.sh', '--no-gui', '-j', '2', *args], **kwargs)

def check(name, condition):
    if not condition:
        raise AssertionError(f'{name}; see {log}')
    checks.append(name)
    print(f'[PASS] build.sh: {name}', flush=True)

def times():
    return {str(p.relative_to(base)): p.stat().st_mtime_ns for p in base.rglob('*.o')}

def value():
    binary = base / 'src/utun'
    if not binary.exists():
        binary = binary.with_suffix('.exe')
    return run([str(binary)]).stdout.strip()

try:
    shutil.copyfile(script, base / 'build.sh')
    write('tools/build_openssl.sh', (script.parent / 'tools/build_openssl.sh').read_text())
    write('tools/prepare_dependencies.py', '''import pathlib, sys
assert sys.argv[1:4] == ['--only', sys.argv[2], '--if-needed'] and sys.argv[2] in ('common', 'sodium', 'dnsmasq', 'wintun'), sys.argv
if sys.argv[2] == 'sodium': assert len(sys.argv) == 6 and sys.argv[4] == '--sodium-target' and sys.argv[5]
else: assert len(sys.argv) == 4
with pathlib.Path('dependency-preparation.log').open('a') as log:
    log.write(sys.argv[2] + '\\n')
if pathlib.Path('fail-dependency-preparation').exists() or pathlib.Path('fail-' + sys.argv[2] + '-preparation').exists():
    raise SystemExit('ERROR: dependency preparation failed')
''')
    write('tools/chatgui/install_deps_win.sh', '#!/bin/sh\nexit 0\n')
    actual_configuration = (script.parent / 'configure.ac').read_text()
    hardening_start = actual_configuration.index('if test "x$enable_hardening" = "xyes"; then')
    hardening_end = actual_configuration.index('# ==================== НАДЁЖНЫЙ ДЕТЕКТ WINDOWS', hardening_start)
    hardening_rules = actual_configuration[hardening_start:hardening_end]
    write('configure.ac', '''AC_INIT([build-test], [1.0])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CANONICAL_HOST
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_PROG_CC
AC_PROG_RANLIB
AM_PROG_AR
AM_CONDITIONAL([OS_WINDOWS], [case "$host_os" in mingw*|msys*|cygwin*) true;; *) false;; esac])
AC_ARG_WITH([ffmpeg], [], [], [with_ffmpeg=no])
AC_ARG_ENABLE([hardening], [], [], [enable_hardening=no])
__HARDENING_RULES__
TEST_CONFIGURE_OPTION
AC_CONFIG_FILES([Makefile lib/Makefile lib/libopus/Makefile src/Makefile tests/Makefile])
AC_OUTPUT
'''.replace('__HARDENING_RULES__', hardening_rules))
    write('m4/test.m4', 'AC_DEFUN([TEST_CONFIGURE_OPTION], [AC_DEFINE([TEST_OPTION], [1], [Test option])])\n')
    root_rules = (script.parent / 'Makefile.am').read_text()
    preparation_rules = root_rules[root_rules.index('# Подготовка тестов'):root_rules.index('# Полный комплект лицензий')]
    write('Makefile.am', 'SUBDIRS = lib lib/libopus src tests\n' + preparation_rules)
    write('lib/Makefile.am', 'noinst_LIBRARIES = libfixture.a\nlibfixture_a_SOURCES = fixture.c\n')
    write('lib/fixture.c', 'int fixture(void) { return 0; }\n')
    write('lib/wintun.dll', 'Windows-only runtime fixture\n')
    write('lib/libopus/Makefile.am', '''noinst_LIBRARIES = libopus_internal.a
libopus_internal_a_SOURCES = opus.c
CFLAGS = -g -Og -fno-omit-frame-pointer -fno-pie -fPIC
''')
    write('lib/libopus/opus.c', 'int opus_fixture(void) { return 0; }\n')
    source_rules = (script.parent / 'src/Makefile.am').read_text()
    copy_rules = source_rules[source_rules.index('# Copy binary to project root'):source_rules.index('# Clean\n')]
    write('src/Makefile.am', '''bin_PROGRAMS = utun
utun_SOURCES = main.c worker.c api.h nested.h
AM_CFLAGS = -DLOCAL_OPTION=1
utun_LDADD = ../lib/libfixture.a ../lib/libopus/libopus_internal.a
''' + copy_rules)
    write('src/main.c', '''#include <stdio.h>
#include "api.h"
int worker(void);
int fixture(void);
int opus_fixture(void);
int main(void) { printf("%d\\n", worker() + fixture() + opus_fixture()); return 0; }
''')
    write('src/worker.c', '#include "api.h"\nint worker(void) { return VALUE + LOCAL_OPTION; }\n')
    write('src/api.h', '#include "nested.h"\n')
    write('src/nested.h', '#define VALUE 10\n')
    test_rules = (script.parent / 'tests/Makefile.am').read_text()
    config_rules = test_rules[test_rules.index('# Copy test configs'):test_rules.index('# Test logs directory')]
    write('tests/Makefile.am', 'check_PROGRAMS = probe\nprobe_SOURCES = probe.c\n' + config_rules
          + 'check-local: check-build\n\t./probe$(EXEEXT)\n')
    write('tests/probe.c', '#include <stdio.h>\nint main(void) { puts("probe passed"); return 0; }\n')
    package_tool = write('bin/pkg-config', '''#!/usr/bin/env bash
[[ "${BUILD_TEST_FFMPEG:-0}" = 1 ]] || exit 1
case "$1" in
    --exists) exit 0 ;;
    --modversion) echo 1.0 ;;
    --cflags|--libs-only-L) exit 0 ;;
    *) exit 1 ;;
esac
''')
    package_tool.chmod(0o755)
    environment['PATH'] = str(base / 'bin') + os.pathsep + environment['PATH']

    result = build()
    check('core prepares common dependencies, target libsodium and dnsmasq before configuration',
          (base / 'dependency-preparation.log').read_text() == 'common\nsodium\ndnsmasq\n')
    check('native build does not copy an available Windows DLL', not (base / 'wintun.dll').exists())
    check('fresh build generates, configures and builds dependencies', value() == '11' and (base / 'lib/libopus/libopus_internal.a').exists())
    check('normal build skips tests', not (base / 'tests/probe').exists() and not (base / 'tests/probe.exe').exists())
    initial = times()
    run(['make', 'check-build', '-j2'])
    check('test preparation builds tests without rebuilding the core',
          (base / 'tests/probe.o').exists() and all(times()[name] == stamp for name, stamp in initial.items()))
    check('make check builds and runs the test', 'probe passed' in run(['make', 'check', '-j2']).stdout)
    original_probe = (base / 'tests/probe.c').read_text()
    write('tests/probe.c', original_probe + '\n#error deliberate_test_failure\n')
    check('a broken test does not block the normal build', build().returncode == 0)
    check('a broken test fails when tests are requested', run(['make', 'check', '-j2'], ok=False).returncode != 0)
    write('tests/probe.c', original_probe)
    check('tests rebuild and run after correction', 'probe passed' in run(['make', 'check', '-j2']).stdout)

    uname = shutil.which('uname')
    fake_uname = write('platform-bin/uname', '#!/usr/bin/env bash\nif [[ "$1" = -s ]]; then echo FreeBSD; else exec '
                       + shlex.quote(uname) + ' "$@"; fi\n')
    fake_uname.chmod(0o755)
    (base / 'fail-dnsmasq-preparation').touch()
    result = build(ok=False, extra_environment={'PATH': str(fake_uname.parent) + os.pathsep + environment['PATH'],
                                               'CC': default_compiler, 'MAKE': make_tool})
    check('FreeBSD prepares common, target libsodium and dnsmasq before configuration', result.returncode != 0
          and 'dnsmasq dependency preparation failed' in result.stderr
          and (base / 'dependency-preparation.log').read_text().splitlines()[-3:] == ['common', 'sodium', 'dnsmasq']
          and 'components=common sodium dnsmasq\n' in result.stdout)
    (base / 'fail-dnsmasq-preparation').unlink()

    windows_compiler = write('bin/windows-target-cc', '#!/usr/bin/env bash\nif [[ "$1" = -dumpmachine ]]; then '
                             'echo x86_64-w64-mingw32; else exec ' + shlex.quote(shutil.which(default_compiler)) + ' "$@"; fi\n')
    windows_compiler.chmod(0o755)
    (base / 'fail-wintun-preparation').touch()
    result = build(ok=False, extra_environment={'CC': str(windows_compiler)})
    check('Windows compiler target requests Wintun and stops on its preparation failure', result.returncode != 0
          and 'wintun dependency preparation failed' in result.stderr and 'INFO: configuring:' not in result.stdout
          and (base / 'dependency-preparation.log').read_text().splitlines()[-4:] == ['common', 'sodium', 'dnsmasq', 'wintun'])
    (base / 'fail-wintun-preparation').unlink()
    initial = times()
    result = build()
    check('repeat build does not regenerate, configure or compile', initial == times() and 'INFO: configuring:' not in result.stdout and 'INFO: regenerating' not in result.stdout)
    time.sleep(1.1)
    write('src/nested.h', '#define VALUE 20\n')
    result = build()
    current = times()
    check('transitive header rebuilds both dependent objects', current['src/main.o'] > initial['src/main.o'] and current['src/worker.o'] > initial['src/worker.o'])
    check('header change preserves unrelated objects and configuration', current['lib/fixture.o'] == initial['lib/fixture.o'] and 'INFO: configuring:' not in result.stdout and value() == '21')
    initial = times()
    write('src/unused.h', '#define UNUSED 1\n')
    build()
    check('unrelated header does not recompile', initial == times())

    write('asan_probe.c', 'int main(void) { return 0; }\n')
    asan_supported = run([default_compiler, '-fsanitize=address', '-fno-pie', '-no-pie', 'asan_probe.c', '-o', 'asan_probe'], ok=False).returncode == 0
    modes = ['asan', 'hardened', 'normal'] if asan_supported else ['hardened', 'normal']
    if not asan_supported:
        print('[SKIP] build.sh: ASAN runtime is unavailable in this compiler toolchain', flush=True)
    for mode in modes:
        args = [] if mode == 'normal' else ['--' + mode]
        result = build(*args)
        symbols = run(['nm', '-u', 'src/worker.o']).stdout
        check(f'{mode}: changing mode reconfigures and cleans', 'INFO: configuring:' in result.stdout and 'INFO: cleaning objects:' in result.stdout)
        check(f'{mode}: object instrumentation matches mode', ('__asan_' in symbols) == (mode == 'asan'))
        if mode == 'asan':
            check('ASAN skips tests and preserves unsanitized Opus', not (base / 'tests/probe').exists() and not (base / 'tests/probe.exe').exists() and '__asan_' not in run(['nm', '-u', 'lib/libopus/opus.o']).stdout)
        initial = times()
        result = build(*args)
        check(f'{mode}: repeat build is incremental', initial == times() and 'INFO: configuring:' not in result.stdout and 'INFO: regenerating' not in result.stdout)

    result = build('CFLAGS=-g -O0 -DUSER_OPTION=1')
    check('quoted compiler flags are passed intact through configure', 'CFLAGS = -g -O0 -DUSER_OPTION=1' in (base / 'src/Makefile').read_text())
    initial = times()
    build('CFLAGS=-g -O0 -DUSER_OPTION=1')
    check('repeat build with quoted flags is incremental', initial == times())
    build()
    compiler = shutil.which(default_compiler)
    for directory in ['compiler-first', 'compiler-second']:
        wrapper = write(directory + '/' + default_compiler, '#!/usr/bin/env bash\nexec ' + shlex.quote(compiler) + ' "$@"\n')
        wrapper.chmod(0o755)
    build(extra_environment={'PATH': str(base / 'compiler-first') + os.pathsep + environment['PATH']})
    result = build(extra_environment={'PATH': str(base / 'compiler-second') + os.pathsep + environment['PATH']})
    check('compiler path change is detected even with identical version', 'INFO: configuring:' in result.stdout)
    build()
    time.sleep(1.1)
    write('src/Makefile.am', (base / 'src/Makefile.am').read_text().replace('LOCAL_OPTION=1', 'LOCAL_OPTION=2'))
    result = build()
    check('Makefile.am flag change rebuilds stale objects', 'INFO: regenerating' in result.stdout and value() == '22')
    macro = base / 'm4/test.m4'
    saved_time = macro.stat()
    write('m4/test.m4', macro.read_text().replace('[1]', '[2]'))
    os.utime(macro, ns=(saved_time.st_atime_ns, saved_time.st_mtime_ns))
    build()
    check('macro content change is detected with unchanged timestamp', '#define TEST_OPTION 2' in (base / 'config.h').read_text())
    result = build(extra_environment={'BUILD_TEST_FFMPEG': '1'})
    check('newly available optional dependency reconfigures', 'INFO: configuring:' in result.stdout and '--with-ffmpeg' in run(['./config.status', '--config']).stdout)
    result = build()
    check('disappearing optional dependency reconfigures', 'INFO: configuring:' in result.stdout and '--without-ffmpeg' in run(['./config.status', '--config']).stdout)
    (base / 'src/.deps/worker.Po').unlink()
    result = build()
    check('missing dependencies force rebuild without configure', 'missing_dependencies=1' in result.stdout and 'INFO: configuring:' not in result.stdout)
    write('src/.deps/worker.Po', '# dummy\n')
    result = build()
    check('dummy dependencies force rebuild', 'missing_dependencies=1' in result.stdout)
    write('tests/test_preserve.conf', 'preserve configuration\n')
    write('tests/test_preserve.json', '{"preserve":true}\n')
    result = build('--clean')
    check('explicit clean does not reconfigure', 'INFO: cleaning objects:' in result.stdout and 'INFO: configuring:' not in result.stdout)
    check('in-source clean preserves configuration files', (base / 'tests/test_preserve.conf').exists() and (base / 'tests/test_preserve.json').exists())
    result = build('--full')
    check('explicit full regenerates, configures and rebuilds', 'INFO: regenerating' in result.stdout and 'INFO: configuring:' in result.stdout)
    (base / 'src/Makefile').unlink()
    result = build()
    check('missing child Makefile is recreated', (base / 'src/Makefile').exists() and 'INFO: configuring:' in result.stdout)
    run(['./configure', 'CFLAGS=-O0'])
    result = build()
    check('external configure is detected', 'INFO: configuring:' in result.stdout and 'CFLAGS = -g -O2' in (base / 'src/Makefile').read_text())
    check('conflicting modes fail', build('--asan', '--hardened', ok=False).returncode != 0)
    check('invalid job count fails', build('-j0', ok=False).returncode != 0)
    (base / 'fail-dependency-preparation').touch()
    result = build('--full', ok=False)
    check('dependency preparation failure stops before configure or make', result.returncode != 0
          and 'common dependency preparation failed' in result.stderr and 'INFO: regenerating' not in result.stdout)
    (base / 'fail-dependency-preparation').unlink()
    (base / 'fail-dnsmasq-preparation').touch()
    result = build('--full', ok=False)
    check('dnsmasq preparation failure stops before configure', result.returncode != 0
          and 'dnsmasq dependency preparation failed' in result.stderr and 'INFO: regenerating' not in result.stdout)
    (base / 'fail-dnsmasq-preparation').unlink()

    state = (base / '.utun-build-state').read_bytes()
    original = (base / 'src/worker.c').read_text()
    write('src/worker.c', original + '\n#error deliberate_failure\n')
    check('compiler failure propagates without committing state', build(ok=False).returncode != 0 and (base / '.utun-build-state').read_bytes() == state)
    write('src/worker.c', original)
    check('build recovers from compiler failure', build().returncode == 0 and value() == '22')
    original = (base / 'configure.ac').read_text()
    write('configure.ac', original + '\nm4_fatal([deliberate_autoconf_failure])\n')
    check('autoreconf failure propagates through tee', build(ok=False).returncode != 0 and (base / '.utun-build-state').read_bytes() == state)
    write('configure.ac', original)
    build()
    write('tools/chatgui/build/CMakeCache.txt', 'placeholder\n')
    cmake = write('bin/cmake', '#!/usr/bin/env bash\nprintf "%s\\n" "$*" > gui-arguments\nexit 9\n')
    cmake.chmod(0o755)
    write('tools/chatgui/build_nix.sh', '#!/bin/sh\nprintf "%s\\n" "$*" > gui-arguments\nexit 9\n')
    result = run(['bash', 'build.sh', '-j', '3'], ok=False)
    check('GUI failure propagates and success is not printed', result.returncode != 0 and 'Build completed successfully!' not in result.stdout)
    check('GUI receives selected job count through its preparation entry point', (base / 'gui-arguments').read_text().strip() == '-j3')
    (base / 'tools/chatgui/build/CMakeCache.txt').unlink()
    write('tools/chatgui/package_win.sh', '''#!/usr/bin/env bash
printf '%s\\n' "$*" > installer-preflight
[[ "${MSYSTEM:-}" = UCRT64 ]] || { echo 'ERROR: installer requires UCRT64'; exit 1; }
''')
    write('tools/chatgui/build.sh', '#!/usr/bin/env bash\nprintf "%s\\n" "$*" > gui-installer-arguments\nexit 9\n')
    result = run(['bash', 'build.sh', '--installer', '-j5'], ok=False, extra_environment={'MSYSTEM': 'UCRT64'})
    check('MSYS2 prepares Wintun before the Windows build',
          (base / 'dependency-preparation.log').read_text().splitlines()[-4:] == ['common', 'sodium', 'dnsmasq', 'wintun'])
    check('installer builds GUI without an existing CMake cache',
          (base / 'tools/chatgui/gui-installer-arguments').read_text().strip() == '-j5 --installer')
    check('installer preflight runs before building', (base / 'installer-preflight').read_text().strip() == '--check')
    check('installer failure propagates through the root build', result.returncode != 0 and 'Build completed successfully!' not in result.stdout)
    write('tools/chatgui/package_deb.sh', '''#!/usr/bin/env bash
printf '%s\\n' "$*" > deb-preflight
''')
    write('tools/chatgui/build_nix.sh', '#!/usr/bin/env bash\nprintf "%s\\n" "$*" > deb-arguments\nexit 9\n')
    if sys.platform.startswith('linux'):
        for flag in ('--installer', '--deb'):
            result = run(['bash', 'build.sh', flag, '-j3'], ok=False)
            check(f'{flag}: Linux preflight and GUI build without a CMake cache',
                  (base / 'deb-preflight').read_text().strip() == '--check'
                  and (base / 'deb-arguments').read_text().strip() == '--deb -j3')
            check(f'{flag}: Linux packaging failure propagates', result.returncode != 0 and 'Build completed successfully!' not in result.stdout)
    result = run(['bash', 'build.sh', '--deb'], ok=False, extra_environment={'MSYSTEM': 'UCRT64'})
    check('DEB rejects a Windows build environment', result.returncode != 0 and '--deb requires Linux' in result.stderr)
    result = run(['bash', 'build.sh', '--installer', '--no-gui'], ok=False)
    check('installer rejects a disabled GUI', result.returncode != 0 and '--installer requires GUI' in result.stderr)

    if sys.platform.startswith('linux'):
        openssl_root = base / 'openssl'
        write('openssl/include/openssl/hpke.h', '/* HPKE header fixture */\n')
        write('openssl/lib64/libcrypto.a', '')
        openssl_environment = {'OPENSSL_ROOT_DIR': str(openssl_root), 'CPPFLAGS': '-DKEEP_CPP=1', 'LDFLAGS': '-Wl,--as-needed'}
        result = build(extra_environment=openssl_environment)
        check('OpenSSL selection preserves user flags and supports lib64',
              f'using OpenSSL from {openssl_root}' in result.stdout
              and f'CPPFLAGS = -I{openssl_root}/include -DKEEP_CPP=1' in (base / 'src/Makefile').read_text()
              and f'-L{openssl_root}/lib64 -Wl,-rpath,{openssl_root}/lib64 -Wl,--as-needed' in (base / 'src/Makefile').read_text())
        initial = times()
        result = build(extra_environment=openssl_environment)
        check('automatic OpenSSL selection keeps repeat builds incremental',
              initial == times() and 'INFO: configuring:' not in result.stdout)
        inherited_environment = {**openssl_environment,
                                 'CPPFLAGS': f'-I{openssl_root}/include -DKEEP_CPP=1',
                                 'LDFLAGS': f'-L{openssl_root}/lib64 -Wl,-rpath,{openssl_root}/lib64 -Wl,--as-needed'}
        result = build(extra_environment=inherited_environment)
        check('inherited OpenSSL paths are not duplicated or reconfigured',
              initial == times() and 'INFO: configuring:' not in result.stdout)
        (openssl_root / 'include/openssl/hpke.h').unlink()
        result = build(ok=False, extra_environment=openssl_environment)
        check('incomplete OpenSSL stops before dependency preparation and configuration',
              result.returncode != 0 and 'incomplete OpenSSL' in result.stderr and 'dependency components:' not in result.stdout)
        build()

    run(['make', 'distclean'])
    output_dir = base / 'out'
    output_dir.mkdir()
    for args in [['../configure'], [make_tool, '-j2'], [make_tool, 'check', '-j2'], [make_tool, '-C', 'tests', 'clean-local']]:
        result = subprocess.run(args, cwd=output_dir, env=environment, capture_output=True, text=True)
        with log.open('a') as output:
            output.write(f'OUT-OF-TREE {args!r}\nexit={result.returncode}\n{result.stdout}{result.stderr}\n')
        if result.returncode:
            raise AssertionError(f'out-of-tree {args!r}: see {log}')
        if args == [make_tool, '-j2']:
            check('out-of-tree normal build skips tests and test configurations',
                  not (output_dir / 'tests/probe.o').exists() and not (output_dir / 'tests/test_preserve.conf').exists())
        if 'check' in args:
            check('out-of-tree check builds tests, copies configurations and runs them',
                  (output_dir / 'tests/test_preserve.conf').exists() and 'probe passed' in result.stdout)
    check('out-of-tree clean removes copies and preserves source configurations',
          not (output_dir / 'tests/test_preserve.conf').exists() and (base / 'tests/test_preserve.conf').exists())
    print(f'[PASS] build.sh: {len(checks)} checks completed', flush=True)
    shutil.rmtree(base)
except Exception:
    print(f'[FAIL] build.sh: diagnostic files retained in {base}', file=sys.stderr)
    raise
