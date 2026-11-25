#!/usr/bin/env python3
"""Проверка полного Linux-скрипта без установки пакетов и сборки рабочего дерева."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class LinuxFullBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="utun-full-build-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "commands.log"
        shutil.copyfile(ROOT / "build_linux_full.sh", self.root / "build_linux_full.sh")
        (self.root / "tools").mkdir()
        shutil.copyfile(ROOT / "tools/build_openssl.sh", self.root / "tools/build_openssl.sh")
        self.env = dict(os.environ, PATH=str(self.bin), TEST_LOG=str(self.log))
        for name in ("CPPFLAGS", "LDFLAGS", "CC", "CXX", "OPENSSL_ROOT_DIR", "CMAKE_PREFIX_PATH"):
            self.env.pop(name, None)
        for name in ("bash", "dirname", "mktemp", "rm", "cat"):
            (self.bin / name).symlink_to(shutil.which(name))
        for name in ("curl", "tar", "patch", "make", "g++", "autoreconf", "automake", "libtoolize", "pkg-config", "cmake"):
            self.tool(name, "exit 0\n")
        self.tool("uname", "echo Linux\n")
        self.tool("id", "echo 0\n")
        self.tool("python3", 'echo "dependencies:$*" >> "$TEST_LOG"\n')
        self.tool("apt-get", 'echo "apt:$*" >> "$TEST_LOG"\nexit 19\n')
        self.tool("gcc", 'if [ "${BAD_OPENSSL:-0}" = 1 ]; then echo "hpke.h unavailable" >&2; exit 1; fi\n'
                  'while [ "$#" -gt 0 ]; do if [ "$1" = -o ]; then shift; : > "$1"; fi; shift; done\n')
        self.write("build.sh", 'echo "core:$*" >> "$TEST_LOG"\n')
        self.write("tools/chatgui/build_nix.sh", 'echo "gui:$*" >> "$TEST_LOG"\n')

    def write(self, name, body):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)

    def tool(self, name, body):
        self.write("bin/" + name, body)

    def run_build(self, *arguments, **env):
        return subprocess.run([shutil.which("bash"), str(self.root / "build_linux_full.sh"), *arguments],
                              cwd="/tmp", env={**self.env, **env}, text=True, capture_output=True)

    def test_full_build_order_and_jobs(self):
        result = self.run_build("-j3")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.log.read_text().splitlines(), [
            "core:--full --no-gui -j3", "gui:--full -j3"])

    def test_core_only_skips_rlottie_and_gui(self):
        result = self.run_build("--no-gui")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.log.read_text().splitlines(), ["core:--full --no-gui -j4"])

    def test_installed_openssl_is_selected_without_manual_flags(self):
        prefix = self.root / "openssl"
        (prefix / "include/openssl").mkdir(parents=True)
        (prefix / "include/openssl/hpke.h").touch()
        (prefix / "lib").mkdir()
        (prefix / "lib/libcrypto.so").touch()
        # Подменяем только место установки в копии рецепта, не трогая системный /opt.
        recipe = self.root / "tools/build_openssl.sh"
        recipe.write_text(recipe.read_text().replace("/opt/openssl-3.5", str(prefix)))
        self.write("build.sh", 'echo "$CPPFLAGS|$LDFLAGS|$OPENSSL_ROOT_DIR" >> "$TEST_LOG"\n')
        result = self.run_build("--no-gui")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"using OpenSSL from {prefix}", result.stdout)
        self.assertEqual(self.log.read_text().strip(), f"-I{prefix}/include|-L{prefix}/lib -Wl,-rpath,{prefix}/lib|{prefix}")

    def test_openssl_paths_reach_core_and_gui_without_duplicates(self):
        prefix = self.root / "openssl"
        (prefix / "include/openssl").mkdir(parents=True)
        (prefix / "include/openssl/hpke.h").touch()
        (prefix / "lib64").mkdir()
        (prefix / "lib64/libcrypto.a").touch()
        self.write("build.sh", '. tools/build_openssl.sh\necho "core:$CPPFLAGS|$LDFLAGS|$CMAKE_PREFIX_PATH" >> "$TEST_LOG"\n')
        self.write("tools/chatgui/build_nix.sh", 'echo "gui:$CPPFLAGS|$LDFLAGS|$CMAKE_PREFIX_PATH" >> "$TEST_LOG"\n')
        result = self.run_build(OPENSSL_ROOT_DIR=str(prefix), CPPFLAGS="-DKEEP=1", LDFLAGS="-Wl,--as-needed",
                                CMAKE_PREFIX_PATH="/custom")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        flags = f"-I{prefix}/include -DKEEP=1|-L{prefix}/lib64 -Wl,-rpath,{prefix}/lib64 -Wl,--as-needed|{prefix}:/custom"
        self.assertEqual(self.log.read_text().splitlines(), ["core:" + flags, "gui:" + flags])

    def test_incomplete_openssl_stops_before_build(self):
        prefix = self.root / "incomplete-openssl"
        prefix.mkdir()
        result = self.run_build(OPENSSL_ROOT_DIR=str(prefix))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"incomplete OpenSSL in {prefix}", result.stderr)
        self.assertIn("hpke.h", result.stderr)
        self.assertFalse(self.log.exists())

    def test_missing_tools_and_failed_install_are_explained(self):
        (self.bin / "patch").unlink()
        result = self.run_build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing build tools: patch", result.stdout)
        self.assertIn("apt could not install", result.stderr)
        self.assertNotIn("dependencies:", self.log.read_text())

    def test_incompatible_openssl_names_the_fix(self):
        result = self.run_build(BAD_OPENSSL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OpenSSL development headers/library with HPKE", result.stderr)
        self.assertIn("tools/install_openssl_ubuntu.sh", result.stderr)
        self.assertFalse(self.log.exists())

    def test_dependency_failure_stops_before_build(self):
        self.tool("python3", 'echo "ERROR: dependency download failed" >&2\nexit 23\n')
        self.write("build.sh", 'python3 tools/prepare_dependencies.py --only common --if-needed\n')
        result = self.run_build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dependency download failed", result.stderr)
        self.assertFalse(self.log.exists())

    def test_core_failure_stops_before_gui(self):
        self.write("build.sh", 'echo "ERROR: core compile failed" >&2\nexit 17\n')
        result = self.run_build()
        self.assertEqual(result.returncode, 17)
        self.assertIn("core compile failed", result.stderr)
        self.assertFalse(self.log.exists())

    def test_gui_package_failure_is_explained_without_sudo_for_root(self):
        shutil.copyfile(ROOT / "tools/chatgui/build_nix.sh", self.root / "tools/chatgui/build_nix.sh")
        result = self.run_build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("apt update failed", result.stderr)
        self.assertNotIn("sudo: command not found", result.stderr)
        self.assertNotIn("build complete:", result.stdout)

    def test_invalid_job_count_stops_before_any_action(self):
        result = self.run_build("-j0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("job count must be positive", result.stderr)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
