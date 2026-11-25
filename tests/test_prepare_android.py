#!/usr/bin/env python3
"""Android-подготовка: SDK bootstrap, отказы, пути проекта и последующая сборка без устройства."""

import contextlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import zipfile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_android as android


class AndroidPreparationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="utun-android-setup-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / "tools/chatgui-android"
        (self.project / "app").mkdir(parents=True)
        (self.project / "app/build.gradle.kts").write_text('compileSdk = 36\nndkVersion = "29.0.14206865"\n')
        self.sdk = self.root / "sdk"
        self.manager = self.sdk / "cmdline-tools/latest/bin/sdkmanager"
        self.markers = ["platform-tools/adb", "platforms/android-36/android.jar", "build-tools/35.0.0/aapt2",
                        "ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/bin/clang", "cmake/3.22.1/bin/cmake"]
        self.touch(self.manager)
        self.install_packages()
        self.commands = []

    def touch(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()

    def install_packages(self):
        for name in self.markers:
            self.touch(self.sdk / name)

    def fake_run(self, command, **kwargs):
        command = [str(arg) for arg in command]
        self.commands.append(command)
        if command[0] == str(self.manager) and "--licenses" not in command:
            self.install_packages()

    def prepare(self, run=None, download=None):
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(android, "REPO", self.root))
            stack.enter_context(mock.patch.object(android, "PROJECT", self.project))
            stack.enter_context(mock.patch.object(android, "java_home", return_value=self.root / "jdk"))
            stack.enter_context(mock.patch.object(android.platform, "system", return_value="Linux"))
            stack.enter_context(mock.patch.object(android.platform, "machine", return_value="x86_64"))
            stack.enter_context(mock.patch.object(android.shutil, "which", return_value="/tool"))
            stack.enter_context(mock.patch.object(android.deps, "run", side_effect=run or self.fake_run))
            fetch = stack.enter_context(mock.patch.object(android.deps, "download", side_effect=download))
            stack.enter_context(mock.patch.object(sys, "argv", ["prepare_android.py", "--sdk", str(self.sdk), "--jobs", "3", "--if-needed"]))
            android.main()
            return fetch

    def test_existing_sdk_prepares_native_dependencies_and_gradle(self):
        (self.project / "local.properties").write_text("# keep\ncustom.setting=value\nsdk.dir=/old\nndk.dir=/old-ndk\n")
        fetch = self.prepare()
        fetch.assert_not_called()
        self.assertEqual([command[command.index("--only") + 1] for command in self.commands[:-1]], ["common", "android"])
        self.assertTrue(all("--if-needed" in command for command in self.commands[:-1]))
        self.assertEqual(self.commands[-1][-1], "--version")
        self.assertIn("custom.setting=value", (self.project / "local.properties").read_text())
        self.assertEqual(android.properties(self.project / "local.properties")["sdk.dir"], str(self.sdk))
        self.assertNotIn("ndk.dir", android.properties(self.project / "local.properties"))

    def test_missing_package_requests_license_before_install(self):
        (self.sdk / self.markers[0]).unlink()
        self.prepare()
        self.assertEqual(self.commands[0][-1], "--licenses")
        self.assertEqual(self.commands[1][-1], "platform-tools")
        self.assertTrue((self.sdk / self.markers[0]).is_file())

    def test_bootstrap_installs_executable_sdkmanager(self):
        self.manager.unlink()
        archive = self.root / "tools.zip"
        with zipfile.ZipFile(archive, "w") as out:
            out.writestr("cmdline-tools/bin/sdkmanager", "#!/bin/sh\nexit 0\n")
        fetch = self.prepare(download=lambda *args: archive)
        self.assertEqual(fetch.call_args.args[0], "android-commandlinetools.zip")
        self.assertTrue(os.access(self.manager, os.X_OK))

    def test_declined_or_failed_install_has_clear_component_error(self):
        (self.sdk / self.markers[0]).unlink()
        with self.assertRaisesRegex(RuntimeError, "platform-tools.*лицензий"):
            self.prepare(run=lambda command, **kwargs: None)
        self.assertFalse((self.project / "local.properties").exists())

    def test_native_failure_does_not_claim_ready_or_run_gradle(self):
        def fail(command, **kwargs):
            self.commands.append([str(arg) for arg in command])
            raise subprocess.CalledProcessError(23, command)
        with self.assertRaises(subprocess.CalledProcessError):
            self.prepare(run=fail)
        self.assertEqual(len(self.commands), 1)

    def test_properties_round_trip_spaces_and_preserve_other_settings(self):
        with mock.patch.object(android, "PROJECT", self.project):
            android.configure_project(self.root / "SDK with spaces", self.root / "cmake")
        self.assertEqual(android.properties(self.project / "local.properties")["sdk.dir"], str(self.root / "SDK with spaces"))

    def test_unsupported_java_version_is_explained(self):
        jdk = self.root / "jdk"
        for name in ("java", "javac"):
            self.touch(jdk / "bin" / name)
        for version in ("11", "24"):
            with self.subTest(version=version), mock.patch.dict(os.environ, {"JAVA_HOME": str(jdk)}), \
                    mock.patch.object(android.subprocess, "run", return_value=types.SimpleNamespace(stdout="", stderr=f'java version "{version}"')):
                with self.assertRaisesRegex(RuntimeError, "JDK 17–23"):
                    android.java_home()

    def test_build_uses_configured_sdk_without_environment(self):
        for name in ("prepare_android.py", "prepare_dependencies.py"):
            shutil.copyfile(ROOT / "tools" / name, self.root / "tools" / name)
        shutil.copyfile(ROOT / "tools/chatgui-android/build.sh", self.project / "build.sh")
        (self.project / "local.properties").write_text(f"sdk.dir={self.sdk}\n")
        (self.project / "prepare_build.sh").write_text('#!/bin/sh\nprintf "%s" "$*" > prepared-arguments\n')
        wrapper = self.project / "gradlew"
        wrapper.write_text('#!/bin/sh\ntest -f prepared-arguments || exit 29\nprintf "%s" "$ANDROID_HOME" > selected-sdk\n'
                           'mkdir -p app/build/outputs/apk/debug\n: > app/build/outputs/apk/debug/app-debug.apk\n')
        wrapper.chmod(0o755)
        env = {key: value for key, value in os.environ.items() if key not in ("ANDROID_HOME", "ANDROID_SDK_ROOT")}
        result = subprocess.run(["bash", str(self.project / "build.sh"), "noinstall"], env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.project / "selected-sdk").read_text(), str(self.sdk))
        self.assertEqual((self.project / "prepared-arguments").read_text(), "--if-needed")

    def test_preparation_failure_stops_before_gradle(self):
        for name in ("prepare_android.py", "prepare_dependencies.py"):
            shutil.copyfile(ROOT / "tools" / name, self.root / "tools" / name)
        shutil.copyfile(ROOT / "tools/chatgui-android/build.sh", self.project / "build.sh")
        (self.project / "prepare_build.sh").write_text('#!/bin/sh\necho "ERROR: SDK preparation failed" >&2\nexit 23\n')
        (self.project / "gradlew").write_text('#!/bin/sh\ntouch unexpected-gradle-call\n')
        env = {key: value for key, value in os.environ.items() if key not in ("ANDROID_HOME", "ANDROID_SDK_ROOT")}
        result = subprocess.run(["bash", str(self.project / "build.sh"), "noinstall"], env=env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Android preparation failed; APK build not started", result.stderr)
        self.assertFalse((self.project / "unexpected-gradle-call").exists())


if __name__ == "__main__":
    unittest.main()
