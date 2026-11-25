#!/usr/bin/env python3
"""Офлайн-проверки скачивания: SHA-256, кеш, частичные файлы и встраивание модели."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_dependencies", ROOT / "tools/prepare_dependencies.py")
DEPS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPS)


class PrepareDependenciesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="utun-deps-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / "cache"
        self.cache.mkdir()
        self.source = self.root / "source.bin"
        self.data = b"dependency fixture\x00\xff"
        self.source.write_bytes(self.data)
        self.expected = hashlib.sha256(self.data).hexdigest()
        DEPS.DOWNLOADS["fixture"] = (self.source.as_uri(), self.expected)
        self.addCleanup(DEPS.DOWNLOADS.pop, "fixture")

    @unittest.skipUnless(shutil.which("curl"), "download tests require curl")
    def test_download_and_offline_cache(self):
        target = DEPS.download("fixture", self.cache, False)
        self.assertEqual(target.read_bytes(), self.data)
        self.source.unlink()
        # Повторный запуск действительно обходит отсутствующий источник.
        self.assertEqual(DEPS.download("fixture", self.cache, True), target)

    def test_download_runs_curl_resolved_from_path(self):
        curl = self.root / "build tools" / "curl.exe"

        def download_fixture(command, **kwargs):
            Path(command[-1]).write_bytes(self.data)

        with mock.patch.object(DEPS.shutil, "which", return_value=str(curl)) as which, \
                mock.patch.object(DEPS.subprocess, "run", side_effect=download_fixture) as run:
            DEPS.download("fixture", self.cache, False)

        which.assert_called_once_with("curl")
        self.assertEqual(Path(run.call_args.args[0][0]), curl)

    @unittest.skipUnless(shutil.which("curl"), "download tests require curl")
    def test_wrong_download_is_never_installed(self):
        self.source.write_bytes(b"truncated or substituted response")
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            DEPS.download("fixture", self.cache, False)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_corrupted_cache_is_rejected_and_preserved(self):
        target = self.cache / "fixture"
        target.write_bytes(b"bad cache")
        for offline in (False, True):
            with self.assertRaisesRegex(RuntimeError, "SHA-256"):
                DEPS.download("fixture", self.cache, offline)
            self.assertEqual(target.read_bytes(), b"bad cache")

    def test_missing_offline_cache(self):
        with self.assertRaisesRegex(RuntimeError, "офлайн-кеше"):
            DEPS.download("fixture", self.cache, True)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_common_preparation_never_downloads_or_tracks_wintun(self):
        sqlite = self.cache / "sqlite.zip"
        with zipfile.ZipFile(sqlite, "w") as archive:
            for name in ("sqlite3.c", "sqlite3.h"):
                archive.writestr(f"sqlite-amalgamation-3530200/{name}", self.data)
        sources = {"sqlite.zip": sqlite, "miniaudio.h": self.source, "silero_vad.onnx": self.source}
        work = self.root / "work"
        work.mkdir()
        with mock.patch.object(DEPS, "download", side_effect=lambda name, *args: sources[name]) as download, \
                mock.patch.object(DEPS, "run"):
            outputs = DEPS.prepare_common(self.root / "output", self.cache, True, work)
        self.assertEqual([call.args[0] for call in download.call_args_list], list(sources))
        self.assertEqual(len(outputs), 7)
        self.assertTrue(all(path.is_file() for path in outputs))
        self.assertFalse((self.root / "output/lib/wintun.dll").exists())

    @unittest.skipUnless(shutil.which("curl"), "download tests require curl")
    def test_wintun_preparation_is_explicit_cached_and_repairs_deleted_dll_offline(self):
        archive = self.root / "wintun.zip"
        with zipfile.ZipFile(archive, "w") as stream:
            stream.writestr("wintun/bin/amd64/wintun.dll", self.data)
        expected = DEPS.digest(archive)
        target = self.root / "output/lib/wintun.dll"
        arguments = ["prepare_dependencies.py", "--only", "wintun", "--output", str(self.root / "output"),
                     "--cache", str(self.cache), "--if-needed"]
        with mock.patch.dict(DEPS.DOWNLOADS, {"wintun.zip": (archive.as_uri(), expected)}), \
                mock.patch.object(DEPS, "prepare_common") as common, mock.patch.object(DEPS, "prepare_android") as android, \
                mock.patch.object(DEPS, "prepare_rlottie") as rlottie:
            with mock.patch.object(DEPS.sys, "argv", arguments):
                DEPS.main()
            self.assertEqual(target.read_bytes(), self.data)
            state = json.loads((self.root / "output/.cache/dependencies/wintun.json").read_text())
            self.assertEqual(state["files"], {"lib/wintun.dll": self.expected})
            archive.unlink()
            target.unlink()
            with mock.patch.object(DEPS.sys, "argv", [*arguments, "--offline"]):
                DEPS.main()
            self.assertEqual(target.read_bytes(), self.data)
            with mock.patch.object(DEPS.sys, "argv", [*arguments, "--offline"]), mock.patch.object(DEPS, "download") as download:
                DEPS.main()
                download.assert_not_called()
            common.assert_not_called()
            android.assert_not_called()
            rlottie.assert_not_called()

    @unittest.skipUnless(shutil.which("tar"), "rlottie extraction requires tar")
    def test_rlottie_extracts_local_archive_with_colon_and_spaces_in_cache_path(self):
        # Двоеточие в пути воспроизводит трактовку C:/... как удалённого архива GNU tar.
        cache = self.root / ("C:" if os.name != "nt" else "local") / "dependency cache"
        cache.mkdir(parents=True)
        archive = cache / "rlottie.tar.gz"
        with tarfile.open(archive, "w:gz") as stream:
            stream.add(self.source, arcname="rlottie-fixture/src/animation.cpp")
        work = self.root / "work"
        work.mkdir()
        run = DEPS.run
        def extract(command, cwd=None, env=None):
            if command[0] == "tar":
                run(command, cwd=cwd, env=env)
        archive_argument = archive.relative_to(self.root) if os.name != "nt" else archive
        previous = Path.cwd()
        try:
            os.chdir(self.root)
            with mock.patch.object(DEPS, "download", return_value=archive_argument), \
                    mock.patch.object(DEPS, "run", side_effect=extract):
                outputs = DEPS.prepare_rlottie(self.root / "output", cache, True, work, 4)
        finally:
            os.chdir(previous)
        target = self.root / "output/tools/chatgui/rlottie_src/src/animation.cpp"
        self.assertEqual(target.read_bytes(), self.data)
        self.assertIn(target, outputs)

    def test_model_generation_logs_utf8_when_parent_requests_cp1251(self):
        header = self.root / "model.inc"
        script = ('import runpy, sys; from pathlib import Path; '
                  'deps = runpy.run_path(sys.argv[1]); deps["configure_output"](); '
                  'deps["generate_model"](Path(sys.argv[2]), Path(sys.argv[3])); '
                  'print("Ошибка → подробности", file=sys.stderr)')
        result = subprocess.run([sys.executable, "-c", script, str(ROOT / "tools/prepare_dependencies.py"),
                                 str(self.source), str(header)], capture_output=True,
                                env={**os.environ, "PYTHONIOENCODING": "cp1251:strict", "PYTHONUTF8": "0"})
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", errors="replace"))
        self.assertIn("ONNX → C:", result.stdout.decode("utf-8"))
        self.assertEqual(result.stderr.decode("utf-8").strip(), "Ошибка → подробности")
        self.assertTrue(header.is_file())

    def test_cli_stdout_and_errors_use_utf8_with_legacy_windows_encoding(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools/prepare_dependencies.py"), "--only", "dnsmasq",
                                 "--output", str(self.root / "output"), "--cache", str(self.cache), "--offline"],
                                capture_output=True,
                                env={**os.environ, "PYTHONIOENCODING": "cp1251:strict", "PYTHONUTF8": "0"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Компоненты=dnsmasq", result.stdout.decode("utf-8"))
        self.assertIn("Нет файла в офлайн-кеше", result.stderr.decode("utf-8"))
        self.assertNotIn("Traceback", result.stderr.decode("utf-8"))

    def test_missing_compiler_fails_before_download_or_build(self):
        for variable, value in (("CC", "missing-c-compiler"), ("CXX", "missing-cxx-compiler")):
            with self.subTest(variable=variable), mock.patch.dict(DEPS.os.environ, {variable: value}), \
                    mock.patch.object(DEPS.sys, "argv", ["prepare_dependencies.py", "--only", "rlottie", "--offline"]), \
                    mock.patch.object(DEPS.shutil, "which", side_effect=lambda name: None if name == value else "/tool"), \
                    mock.patch.object(DEPS, "run") as run:
                with self.assertRaisesRegex(RuntimeError, value):
                    DEPS.main()
                run.assert_not_called()

    def prepare_fixture(self, prepare):
        arguments = ["prepare_dependencies.py", "--only", "common", "--output", str(self.root / "output"),
                     "--cache", str(self.cache), "--offline", "--if-needed"]
        with mock.patch.object(DEPS.sys, "argv", arguments), mock.patch.object(DEPS, "prepare_common", side_effect=prepare), \
                mock.patch.object(DEPS.shutil, "which", return_value="/tool"):
            DEPS.main()

    def test_prepared_outputs_skip_work_and_repair_deleted_or_modified_files(self):
        target = self.root / "output/lib/dependency.h"
        def prepare(*args):
            return [DEPS.install(self.data, target)]
        with mock.patch.object(DEPS, "download") as download:
            self.prepare_fixture(prepare)
            stamp = target.stat().st_mtime_ns
            self.prepare_fixture(mock.Mock(side_effect=AssertionError("unchanged dependency was rebuilt")))
            self.assertEqual(target.stat().st_mtime_ns, stamp)
            target.unlink()
            self.prepare_fixture(prepare)
            self.assertEqual(target.read_bytes(), self.data)
            target.write_bytes(b"corrupted dependency")
            self.prepare_fixture(prepare)
            self.assertEqual(target.read_bytes(), self.data)
            download.assert_not_called()

    def test_changed_patch_invalidates_preparation(self):
        patches = self.root / "patches"
        patches.mkdir()
        patch = patches / "miniaudio.patch"
        patch.write_text("initial recipe")
        prepare = mock.Mock(side_effect=lambda *args: [DEPS.install(self.data, self.root / "output/lib/dependency.h")])
        with mock.patch.object(DEPS, "PATCHES", patches):
            self.prepare_fixture(prepare)
            patch.write_text("updated recipe")
            self.prepare_fixture(prepare)
        self.assertEqual(prepare.call_count, 2)

    def test_failed_preparation_never_writes_success_state(self):
        with self.assertRaisesRegex(RuntimeError, "preparation failed"):
            self.prepare_fixture(mock.Mock(side_effect=RuntimeError("preparation failed")))
        self.assertFalse((self.root / "output/.cache/dependencies/common.json").exists())

    def test_corrupted_preparation_state_is_rebuilt(self):
        prepare = mock.Mock(side_effect=lambda *args: [DEPS.install(self.data, self.root / "output/lib/dependency.h")])
        self.prepare_fixture(prepare)
        (self.root / "output/.cache/dependencies/common.json").write_text("{incomplete state")
        self.prepare_fixture(prepare)
        self.assertEqual(prepare.call_count, 2)

    def test_default_preparation_never_requests_android(self):
        target = self.root / "output/lib/dependency.h"
        prepare = lambda *args: [DEPS.install(self.data, target)]
        with mock.patch.object(DEPS.sys, "argv", ["prepare_dependencies.py", "--output", str(self.root / "output"), "--offline"]), \
                mock.patch.object(DEPS.shutil, "which", return_value="/tool"), \
                mock.patch.object(DEPS, "prepare_common", side_effect=prepare) as common, \
                mock.patch.object(DEPS, "prepare_rlottie", side_effect=prepare) as rlottie, \
                mock.patch.object(DEPS, "prepare_android") as android, mock.patch.object(DEPS, "download") as download:
            DEPS.main()
        common.assert_called_once()
        rlottie.assert_called_once()
        android.assert_not_called()
        download.assert_not_called()

    @unittest.skipUnless(shutil.which("curl"), "download tests require curl")
    def test_dnsmasq_preparation_downloads_only_its_archive_and_recovers_offline(self):
        arguments = ["prepare_dependencies.py", "--only", "dnsmasq", "--output", str(self.root / "output"),
                     "--cache", str(self.cache), "--if-needed"]
        target = self.root / "output/src/dnsmasq/dnsmasq-2.93.tar.xz"
        with mock.patch.dict(DEPS.DOWNLOADS, {"dnsmasq-2.93.tar.xz": (self.source.as_uri(), self.expected)}), \
                mock.patch.object(DEPS, "prepare_common") as common, mock.patch.object(DEPS, "prepare_android") as android, \
                mock.patch.object(DEPS, "prepare_rlottie") as rlottie:
            with mock.patch.object(DEPS.sys, "argv", arguments):
                DEPS.main()
            self.assertEqual(target.read_bytes(), self.data)
            self.source.unlink()
            target.unlink()
            with mock.patch.object(DEPS.sys, "argv", [*arguments, "--offline"]):
                DEPS.main()
            self.assertEqual(target.read_bytes(), self.data)
            with mock.patch.object(DEPS.sys, "argv", [*arguments, "--offline"]), mock.patch.object(DEPS, "download") as download:
                DEPS.main()
                download.assert_not_called()
            common.assert_not_called()
            android.assert_not_called()
            rlottie.assert_not_called()

    @unittest.skipUnless(shutil.which("cc"), "model compile check requires a C compiler")
    def test_generated_model_compiles_and_preserves_every_byte(self):
        # Все 256 значений и неполная последняя строка: ошибки signed char/длины не скрываются.
        data = bytes(range(256)) * 2 + b"\x00\xff\x7f"
        model = self.root / "model.onnx"
        model.write_bytes(data)
        header = self.root / "silero_vad_model.inc"
        DEPS.generate_model(model, header)
        stamp = header.stat().st_mtime_ns
        DEPS.generate_model(model, header)
        self.assertEqual(header.stat().st_mtime_ns, stamp)
        source = self.root / "check.c"
        source.write_text('#include <stdio.h>\n#include "silero_vad_model.inc"\n'
                          'int main(void) { return fwrite(silero_vad_model_data, 1, silero_vad_model_len, stdout) '
                          '== silero_vad_model_len ? 0 : 1; }\n')
        binary = self.root / "check"
        subprocess.run(["cc", "-std=c99", "-Wall", "-Werror", str(source), "-o", str(binary)], check=True)
        self.assertEqual(subprocess.check_output([str(binary)]), data)


if __name__ == "__main__":
    unittest.main()
