#!/usr/bin/env python3
"""Регрессии запускателя: лимит jobs, CPU-барьеры, ошибки и освобождение дочерних процессов."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from run_tests import available_cpus


@unittest.skipUnless(os.name == "posix", "fixtures use POSIX executables and fork")
class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="utun_runner_")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.runner = Path(__file__).with_name("run_tests.py").resolve()

    def fixture(self, name, body=""):
        path = self.root / name
        path.write_text(f"#!{sys.executable}\n" + """
import json, os, signal, sys, time
from pathlib import Path
name = Path(__file__).name
def event(state):
    fd = os.open('events', os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    os.write(fd, (json.dumps([name, state]) + '\\n').encode()); os.close(fd)
event('start')
""" + body + "\nevent('end')\nprint('fixture output: ' + name)\n")
        path.chmod(0o755)

    def command(self, tests, options=()):
        return [sys.executable, str(self.runner), "--log-dir", "logs", *options, *tests]

    def run_runner(self, tests, options=(), environment=None):
        env = dict(os.environ, TEST_JOBS="2") if environment is None else environment
        return subprocess.run(self.command(tests, options), cwd=self.root, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=15)

    def test_default_cpu_count_and_override(self):
        self.fixture("fast")
        env = os.environ.copy(); env.pop("TEST_JOBS", None)
        result = self.run_runner(["fast"], environment=env)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(f"jobs={available_cpus()} available_cpus=", result.stdout)
        if hasattr(os, "sched_setaffinity"):
            affinity = {min(os.sched_getaffinity(0))}
            result = subprocess.run(self.command(["fast"]), cwd=self.root, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10,
                                    preexec_fn=lambda: os.sched_setaffinity(0, affinity))
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("jobs=1 available_cpus=1", result.stdout)
        result = self.run_runner(["fast"], environment=dict(env, TEST_JOBS="1"))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("jobs=1 available_cpus=", result.stdout)
        result = self.run_runner(["fast"], environment=dict(env, TEST_JOBS="0"))
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("expected a positive integer", result.stdout)

    def test_parallel_limit_and_measurements_after_pool(self):
        for name, pair in (("first_a", "first"), ("first_b", "first"), ("last_a", "last"), ("last_b", "last")):
            self.fixture(name, f"""
Path(name + '.ready').touch()
deadline = time.monotonic() + 5
while len(list(Path('.').glob('{pair}_*.ready'))) < 2:
    if time.monotonic() > deadline: sys.exit(9)
    time.sleep(0.01)
time.sleep(0.05)
""")
        self.fixture("test_media_async", "time.sleep(0.05)\n")
        result = self.run_runner(["first_a", "first_b", "test_media_async", "last_a", "last_b"])
        self.assertEqual(result.returncode, 0, result.stdout)
        active, maximum = set(), 0
        events = [json.loads(line) for line in (self.root / "events").read_text().splitlines()]
        for name, state in events:
            if state == "start":
                if name == "test_media_async": self.assertFalse(active)
                self.assertNotIn("test_media_async", active)
                active.add(name); maximum = max(maximum, len(active))
            else:
                active.remove(name)
        self.assertFalse(active)
        self.assertEqual(maximum, 2)
        self.assertEqual({name for name, _ in events[:2]}, {"first_a", "first_b"})
        self.assertEqual(events[-2:], [["test_media_async", "start"], ["test_media_async", "end"]])
        self.assertIn("test_media_async: running alone to check CPU/wall-time limits", result.stdout)

    def test_long_network_tests_share_pool(self):
        names = ["test_bbr_integration", "test_auto_socket_dynamic"]
        for name in names:
            self.fixture(name, """
Path(name + '.ready').touch()
deadline = time.monotonic() + 5
while len(list(Path('.').glob('*.ready'))) < 2:
    if time.monotonic() > deadline: sys.exit(9)
    time.sleep(0.01)
time.sleep(0.05)
""")
        # У фиктивного autosocket нет требований root: не вызываем sudo в этой регрессии.
        result = self.run_runner(names, environment=dict(os.environ, TEST_JOBS="2", PATH=str(self.root)))
        self.assertEqual(result.returncode, 0, result.stdout)
        events = [json.loads(line) for line in (self.root / "events").read_text().splitlines()]
        self.assertEqual({name for name, _ in events[:2]}, set(names))
        self.assertEqual([state for _, state in events], ["start", "start", "end", "end"])

    def test_local_discovery_runs_alone_after_pool(self):
        names = ["test_local_transport", "test_local_transport_packets"]
        for name in ["ordinary", *names]:
            self.fixture(name, "time.sleep(0.05)\n")
        result = self.run_runner([names[0], "ordinary", names[1]])
        self.assertEqual(result.returncode, 0, result.stdout)
        events = [json.loads(line) for line in (self.root / "events").read_text().splitlines()]
        self.assertEqual(events, [[name, state] for name in ["ordinary", *names] for state in ["start", "end"]])
        for name in names:
            self.assertIn(f"{name}: running alone to isolate the LAN discovery port", result.stdout)

    def test_exit_codes_and_separate_logs(self):
        self.fixture("ok")
        self.fixture("bad", "print('failure detail', flush=True)\nsys.exit(42)\n")
        self.fixture("skip", "sys.exit(77)\n")
        result = self.run_runner(["ok", "bad", "skip", "missing"])
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("1 passed, 1 failed, 2 skipped (4 total)", result.stdout)
        self.assertIn("Failed tests: bad", result.stdout)
        self.assertIn("failure detail", (self.root / "logs/bad.log").read_text())
        self.assertNotIn("failure detail", (self.root / "logs/ok.log").read_text())

    def test_serial_override(self):
        self.fixture("serial_a", "time.sleep(0.05)\n")
        self.fixture("serial_b", "time.sleep(0.05)\n")
        result = self.run_runner(["serial_a", "serial_b"], environment=dict(os.environ, TEST_JOBS="1"))
        self.assertEqual(result.returncode, 0, result.stdout)
        events = [json.loads(line) for line in (self.root / "events").read_text().splitlines()]
        self.assertEqual(events, [["serial_a", "start"], ["serial_a", "end"], ["serial_b", "start"], ["serial_b", "end"]])

    def test_temporary_data_removed_and_logs_replaced_on_success_and_failure(self):
        for name, code in (("good", 0), ("bad", 42)):
            self.fixture(name, f"""
temporary = Path(os.environ['TMPDIR'])
assert temporary.parent == Path('/tmp')
assert str(temporary) == os.environ['TMP'] == os.environ['TEMP']
Path(name + '.temporary').write_text(str(temporary))
(temporary / 'keys.db').write_text('private fixture data')
(temporary / 'nested').mkdir()
(temporary / 'nested/current.log').write_text('first nested log')
import tempfile
external = Path(tempfile.mkdtemp(prefix='utun-registered-', dir='/tmp'))
Path(name + '.external').write_text(str(external))
Path(os.environ['UTUN_TEST_TEMP_REGISTRY']).write_text(str(external) + '\\n')
(external / 'private.db').write_text('discard')
(external / 'external.log').write_text('registered diagnostic')
print('first stdout log', flush=True)
sys.exit({code})
""")
        environment = dict(os.environ, TMPDIR=str(self.root / 'must-not-be-used'))
        result = self.run_runner(["good", "bad"], environment=environment)
        self.assertEqual(result.returncode, 1, result.stdout)
        for name in ("good", "bad"):
            self.assertFalse(Path((self.root / (name + '.temporary')).read_text()).exists())
            external = Path((self.root / (name + '.external')).read_text())
            self.assertFalse(external.exists())
            nested = self.root / 'logs' / (name + '.logs')
            self.assertEqual((nested / 'nested/current.log').read_text(), 'first nested log')
            self.assertEqual((nested / external.name / 'external.log').read_text(), 'registered diagnostic')
            self.assertEqual({path.name for path in nested.rglob('*') if path.is_file()}, {'current.log', 'external.log'})
            (nested / 'stale.log').write_text('old run')
            self.fixture(name, "print('second stdout log', flush=True)\n")
        result = self.run_runner(["good", "bad"])
        self.assertEqual(result.returncode, 0, result.stdout)
        for name in ("good", "bad"):
            self.assertFalse((self.root / 'logs' / (name + '.logs')).exists())
            log = (self.root / 'logs' / (name + '.log')).read_text()
            self.assertIn('second stdout log', log); self.assertNotIn('first stdout log', log)

    def test_successful_parent_does_not_leave_a_descendant(self):
        self.fixture("orphan", """
pid = os.fork()
if pid == 0:
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    Path('child.pid').write_text(str(os.getpid()))
    while True: time.sleep(1)
deadline = time.monotonic() + 5
while not Path('child.pid').exists():
    if time.monotonic() > deadline: sys.exit(9)
    time.sleep(0.01)
""")
        result = self.run_runner(["orphan"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assert_descendant_stopped()

    def stubborn_fixture(self):
        self.fixture("stubborn", """
temporary = Path(os.environ['TMPDIR'])
Path('stubborn.temporary').write_text(str(temporary))
(temporary / 'leftover.db').write_text('discard')
(temporary / 'diagnostic.log').write_text('keep diagnostic')
import tempfile
external = Path(tempfile.mkdtemp(prefix='utun-registered-', dir='/tmp'))
Path('stubborn.external').write_text(str(external))
Path(os.environ['UTUN_TEST_TEMP_REGISTRY']).write_text(str(external) + '\\n')
(external / 'private.db').write_text('discard')
signal.signal(signal.SIGTERM, signal.SIG_IGN)
pid = os.fork()
Path('child.pid' if pid == 0 else 'parent.pid').write_text(str(os.getpid()))
while True: time.sleep(1)
""")

    def assert_descendant_stopped(self):
        pid = int((self.root / "child.pid").read_text())
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
                stat = Path(f"/proc/{pid}/stat")
                if stat.exists() and stat.read_text().split(")", 1)[1].split()[0] == "Z": return
            except ProcessLookupError:
                return
            time.sleep(0.01)
        self.fail(f"descendant pid={pid} survived runner cleanup")

    def test_timeout_stops_descendant(self):
        self.stubborn_fixture()
        result = self.run_runner(["stubborn"], ["--timeout", "1"])
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("timeout=1s", result.stdout)
        self.assertIn("0 passed, 1 failed, 0 skipped", result.stdout)
        self.assert_descendant_stopped()
        self.assertFalse(Path((self.root / 'stubborn.temporary').read_text()).exists())
        self.assertFalse(Path((self.root / 'stubborn.external').read_text()).exists())
        self.assertEqual((self.root / 'logs/stubborn.logs/diagnostic.log').read_text(), 'keep diagnostic')

    def test_interruption_and_log_directory_lock(self):
        self.stubborn_fixture()
        process = subprocess.Popen(self.command(["stubborn"]), cwd=self.root,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 5
            while not (self.root / "child.pid").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue((self.root / "child.pid").exists())
            result = self.run_runner(["stubborn"])
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn("concurrent runs", result.stdout)
            self.assertIsNone(process.poll())
            process.send_signal(signal.SIGTERM)
            output, _ = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 128 + signal.SIGTERM, output)
            self.assertIn("test processes stopped", output)
            self.assert_descendant_stopped()
            self.assertFalse(Path((self.root / 'stubborn.temporary').read_text()).exists())
            self.assertFalse(Path((self.root / 'stubborn.external').read_text()).exists())
            self.assertEqual((self.root / 'logs/stubborn.logs/diagnostic.log').read_text(), 'keep diagnostic')
            self.fixture("ok")
            result = self.run_runner(["ok"])
            self.assertEqual(result.returncode, 0, result.stdout)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM); process.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main()
