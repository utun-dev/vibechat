#!/usr/bin/env python3
"""Сборка патченного Qt: точная версия, SHA-256, повторный запуск и отказ без готового runtime."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tarfile
import tempfile

source = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='vibechat Qt build ') as temporary:
    root = Path(temporary)
    helper = root / 'gui/qt_multimedia'
    helper.mkdir(parents=True)
    for name in ('build.sh', 'camera-error-unicode.patch', 'mingw-constexpr.patch'):
        shutil.copyfile(source / 'qt_multimedia' / name, helper / name)
    commands = root / 'commands'
    commands.mkdir()
    prefix = root / 'ucrt64'
    gui_build = root / 'gui/build'
    fixture = root / 'download'
    fixture.mkdir()
    qt = root / 'archive/qtmultimedia-everywhere-src-6.10.2'

    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    write(qt / 'src/plugins/multimedia/ffmpeg/qwindowscamera.cpp', '''
    void onReadSample(HRESULT status, LONGLONG timestamp, IMFSample *sample)
    {
        if (FAILED(status)) {
            const std::string msg{ std::system_category().message(status) };
            m_windowsCamera.updateError(QCamera::CameraError, QString::fromStdString(msg));
            return;
        }

''')
    write(qt / 'src/multimedia/video/qvideoframeformat.cpp', '''
QT_BEGIN_NAMESPACE

class QVideoFrameFormatPrivate : public QSharedData
{
public:
''')
    write(qt / 'src/multimedia/qmediametadata.cpp', 'QT_BEGIN_NAMESPACE\n\n')
    write(qt / 'LICENSES/LGPL-3.0-only.txt', 'Qt license fixture\n')
    archive = fixture / 'qtmultimedia-everywhere-src-6.10.2.tar.xz'
    with tarfile.open(archive, 'w:xz') as packed:
        packed.add(qt, arcname=qt.name)
    write(Path(str(archive) + '.sha256'), hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')

    def command(name, text):
        write(commands / name, '#!/usr/bin/env python3\n' + text)
        (commands / name).chmod(0o755)

    command('pkg-config', '''import os,sys
name=sys.argv[-1]
print(os.environ.get('CORE_VERSION', '6.10.2') if name=='Qt6Core' else '6.10.2')
''')
    command('pacman', "print('qt6-base 6.10.2-1 qt6-multimedia 6.10.2-1 gcc 15.2.0-1')\n")
    command('curl', '''from pathlib import Path
import os,sys,shutil
url=next(a for a in sys.argv if a.startswith('https://'))
with open(os.environ['CURL_LOG'],'a') as log: log.write(url+'\\n')
out=Path(sys.argv[sys.argv.index('-o')+1]); shutil.copyfile(Path(os.environ['DOWNLOAD'])/url.rsplit('/',1)[1], out)
if os.environ.get('BAD_CHECKSUM') and url.endswith('.sha256'): out.write_text('0'*64+'  qtmultimedia-everywhere-src-6.10.2.tar.xz\\n')
''')
    command('cmake', '''from pathlib import Path
import os,sys
args=sys.argv[1:]
with open(os.environ['CMAKE_LOG'],'a') as log: log.write(repr(args)+'\\n')
if os.environ.get('FAIL_BUILD'): sys.exit(9)
if '-S' in args:
    source=Path(args[args.index('-S')+1])
    camera=(source/'src/plugins/multimedia/ffmpeg/qwindowscamera.cpp').read_text()
    assert 'QSystemError::windowsComString(status)' in camera and 'std::system_category' not in camera
    assert '-DFEATURE_ffmpeg=ON' in args and '-DQT_BUILD_TESTS=OFF' in args
    assert any(a.startswith('-DFFMPEG_DIR=') for a in args)
    build=Path(args[args.index('-B')+1]); build.mkdir(parents=True,exist_ok=True)
    (build/'CMakeCache.txt').write_text('INSTALL_BINDIR:STRING=bin\\nINSTALL_PLUGINSDIR:STRING=share/qt6/plugins\\n')
else:
    assert args[:1]==['--build']
    assert args[args.index('--target')+1:args.index('--parallel')]==['MultimediaWidgets','QFFmpegMediaPlugin']
    build=Path(args[1])
    for name in ('bin/Qt6Multimedia.dll','bin/Qt6MultimediaWidgets.dll','share/qt6/plugins/multimedia/ffmpegmediaplugin.dll'):
        output=build/name; output.parent.mkdir(parents=True,exist_ok=True); output.write_text('patched Qt '+name)
    if os.environ.get('MISSING_PLUGIN'): (build/'share/qt6/plugins/multimedia/ffmpegmediaplugin.dll').unlink()
''')
    environment = {**os.environ, 'MSYSTEM': 'UCRT64', 'MSYSTEM_PREFIX': str(prefix),
                   'PATH': str(commands) + os.pathsep + os.environ['PATH'], 'DOWNLOAD': str(fixture),
                   'CURL_LOG': str(root / 'curl.log'), 'CMAKE_LOG': str(root / 'cmake.log')}

    def run(ok=True, **overrides):
        result = subprocess.run(['bash', str(helper / 'build.sh'), str(gui_build), '2'],
                                env={**environment, **overrides}, text=True, capture_output=True)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        if not ok:
            assert 'ERROR:' in result.stdout + result.stderr
        return result

    run()
    marker = gui_build / 'chatgui_qt_multimedia_runtime.txt'
    runtime = Path(marker.read_text().strip())
    assert (runtime / 'plugins/multimedia/ffmpegmediaplugin.dll').is_file()
    assert '6.10.2' in (runtime / 'licenses/SOURCE.txt').read_text()
    assert (runtime / 'licenses/camera-error-unicode.patch').is_file()
    assert (runtime / 'licenses/LGPL-3.0-only.txt').is_file()
    plugin = runtime / 'plugins/multimedia/ffmpegmediaplugin.dll'
    plugin_stamp = plugin.stat().st_mtime_ns
    downloaded = (root / 'curl.log').read_text()
    run()
    assert (root / 'curl.log').read_text() == downloaded, 'Repeated build downloaded source again'
    assert marker.read_text().strip() == str(runtime)
    assert plugin.stat().st_mtime_ns == plugin_stamp, 'Repeated build changed an identical runtime DLL'
    print('PASS: matching Qt source, verified checksum, Unicode patch, paired DLLs, license notices and incremental rebuild')
    run(ok=False, CORE_VERSION='6.11.2')
    run(ok=False, FAIL_BUILD='1')
    run(ok=False, MISSING_PLUGIN='1')
    assert marker.read_text().strip() == str(runtime), 'Failed build published a different runtime'
    shutil.rmtree(helper / 'build')
    failed = run(ok=False, BAD_CHECKSUM='1')
    assert 'Qt source checksum mismatch' in failed.stderr
    assert not list((helper / 'build').glob('*/source'))
    print('PASS: Qt version mismatch, compiler failure, missing plugin and checksum mismatch fail explicitly')
