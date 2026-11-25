#!/usr/bin/env python3
"""Запуск make check: ограниченный пул, отдельные логи и барьеры для замеров CPU."""

import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


# CPU-замеры и проверки общего LAN discovery-порта выполняем после общего пула по одному.
EXCLUSIVE = {
    "test_media_async": "check CPU/wall-time limits",
    "test_uasync_load": "check CPU/wall-time limits",
    "test_local_transport": "isolate the LAN discovery port",
    "test_local_transport_packets": "isolate the LAN discovery port",
    "test_local_routes": "isolate the LAN discovery port",
    "test_chat_background_async": "isolate the LAN discovery port",
}
EXCLUSIVE.update({name + ".exe": reason for name, reason in list(EXCLUSIVE.items())})


def available_cpus():
    if hasattr(os, "sched_getaffinity"):
        try:
            return max(1, len(os.sched_getaffinity(0)))
        except OSError as error:
            print(f"[WARN] CPU affinity unavailable: {error}; using system CPU count", file=sys.stderr)
    return os.cpu_count() or 1


def positive_int(value):
    try:
        number = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError("expected a positive integer") from None
    if number < 1:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return number


class InterruptedRun(Exception):
    def __init__(self, signum):
        self.signum = signum


# Advisory lock снимается ОС даже при SIGKILL; соседний запуск не перезапишет логи.
def lock_logs(path):
    lock = (path / ".runner.lock").open("a+b")
    try:
        if os.name == "posix":
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        else:
            import msvcrt
            if not lock.tell():
                lock.write(b"\0"); lock.flush()
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        lock.close()
        raise
    return lock


# Останавливаем всю группу теста, включая fork/exec-помощников, с общей паузой 1с.
def stop_processes(processes):
    if os.name != "posix":
        for process in processes:
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            process.wait()
        return
    for process in processes:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 1
    for process in processes:
        try:
            process.wait(timeout=max(0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            pass
    for process in processes:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run_tests(args, requested_signals):
    counts = dict(PASS=0, FAIL=0, SKIP=0)
    failed = []
    running = []
    index = 0
    tests = [name for name in args.tests if name not in EXCLUSIVE] + [name for name in args.tests if name in EXCLUSIVE]
    use_sudo = ("test_auto_socket_dynamic" in args.tests and hasattr(os, "geteuid") and os.geteuid() != 0
                and shutil.which("sudo") and subprocess.run(["sudo", "-n", "true"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0)

    def report(name, status, elapsed, reason=""):
        counts[status] += 1
        if status == "FAIL":
            failed.append(name)
        detail = f" ({reason}; see {args.log_dir / (name + '.log')})" if reason else ""
        print(f"[{status}] {name} ({elapsed * 1000:.0f}ms){detail}", flush=True)

    # Сохраняем только логи; БД/ключи/медиа удаляются также после ошибки, timeout и SIGTERM.
    def cleanup(name, temporary, log):
        destination = args.log_dir / (name + ".logs")
        success = True
        roots = {temporary}
        try:
            registry = temporary / ".artifacts"
            for line in registry.read_text().splitlines() if registry.exists() else []:
                path = Path(line)
                if not path.is_absolute() or ".." in path.parts or (os.name == "posix" and Path("/tmp") not in path.parents):
                    raise ValueError(f"invalid registered temporary path: {line}")
                roots.add(path)
        except (OSError, ValueError) as error:
            log.write(f"[ERROR] runner: cannot read temporary registry: {error}\n".encode()); success = False
        for root in sorted(roots, key=lambda path: len(path.parts), reverse=True):
            try:
                if root.is_symlink():
                    root.unlink(); continue
                if not root.exists():
                    continue
                for source in root.rglob("*") if root.is_dir() else [root]:
                    if source.is_symlink() or not source.is_file():
                        continue
                    if source.suffix != ".log" and ".log." not in source.name and not source.name.startswith(("asan.", "utun_asan.")):
                        continue
                    relative = source.relative_to(root) if root.is_dir() else Path(source.name)
                    target = destination / (Path(root.name) if root != temporary else Path()) / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target)
            except OSError as error:
                log.write(f"[ERROR] runner: cannot preserve nested log path={root}: {error}\n".encode()); success = False
            finally:
                try:
                    if root.is_dir():
                        shutil.rmtree(root)
                    elif root.exists():
                        root.unlink()
                except OSError as error:
                    log.write(f"[ERROR] runner: temporary cleanup failed path={root}: {error}\n".encode()); success = False
        try:
            if destination.exists() and not any(destination.iterdir()):
                destination.rmdir()
        except OSError as error:
            log.write(f"[ERROR] runner: empty log directory cleanup failed path={destination}: {error}\n".encode()); success = False
        log.write(f"[INFO] runner: temporary cleanup complete paths={len(roots)} success={success}\n".encode())
        return success

    print(f"[INFO] test runner: jobs={args.jobs} available_cpus={available_cpus()} timeout={args.timeout}s", flush=True)
    try:
        while index < len(tests) or running:
            if requested_signals:
                raise InterruptedRun(requested_signals[0])
            while index < len(tests) and len(running) < args.jobs:
                if requested_signals:
                    break
                name = tests[index]
                if running and (name in EXCLUSIVE or running[0][0] in EXCLUSIVE):
                    break
                index += 1
                nested_logs = args.log_dir / (name + ".logs")
                if nested_logs.is_symlink():
                    nested_logs.unlink()
                elif nested_logs.exists():
                    shutil.rmtree(nested_logs)
                if not Path(name).is_file():
                    (args.log_dir / (name + ".log")).write_text("[SKIP] runner: test not built\n")
                    report(name, "SKIP", 0, "not built")
                    continue
                nested_logs.mkdir()
                log = (args.log_dir / (name + ".log")).open("wb")
                try:
                    temporary = Path(tempfile.mkdtemp(prefix=f"utun-{name}-", dir="/tmp" if os.name == "posix" else None))
                except OSError as error:
                    log.write(f"[ERROR] runner: cannot create temporary directory: {error}\n".encode()); log.close()
                    nested_logs.rmdir(); report(name, "FAIL", 0, f"temporary directory: {error}"); continue
                environment = dict(os.environ, TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary),
                                   UTUN_TEST_LOG_DIR=str(nested_logs.resolve()), UTUN_TEST_TEMP_REGISTRY=str(temporary / ".artifacts"))
                command = [str(Path(name).resolve())]
                if name in EXCLUSIVE:
                    print(f"[INFO] {name}: running alone to {EXCLUSIVE[name]}", flush=True)
                if name == "test_auto_socket_dynamic" and use_sudo:
                    command = ["sudo", "-n", "--", "env"] + [f"{key}={environment[key]}" for key in
                        ("TMPDIR", "TMP", "TEMP", "UTUN_TEST_LOG_DIR", "UTUN_TEST_TEMP_REGISTRY")] + command
                    print(f"[INFO] {name}: running as root via sudo -n", flush=True)
                log.write(f"[INFO] runner: test={name} exclusive={name in EXCLUSIVE} command={command}\n".encode())
                log.write(f"[INFO] runner: temporary={temporary} nested_logs={nested_logs}\n".encode())
                log.flush()
                started = time.monotonic()
                try:
                    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True, env=environment)
                except OSError as error:
                    log.write(f"[ERROR] cannot start test: {error}\n".encode()); cleanup(name, temporary, log); log.close()
                    report(name, "FAIL", time.monotonic() - started, f"cannot start: {error}")
                    continue
                running.append((name, process, log, started, temporary))
                log.write(f"[INFO] runner: pid={process.pid}\n".encode()); log.flush()
                if name in EXCLUSIVE:
                    break
            for entry in running[:]:
                name, process, log, started, temporary = entry
                elapsed = time.monotonic() - started
                rc = process.poll()
                reason = ""
                if rc is None and elapsed >= args.timeout:
                    stop_processes([process])
                    rc = 124
                    reason = f"timeout={args.timeout}s"
                    log.write(f"[ERROR] runner: {reason}; process group stopped\n".encode())
                if rc is not None:
                    if not reason:
                        stop_processes([process])
                    if not cleanup(name, temporary, log):
                        rc = 1; reason = "temporary/log cleanup failed"
                    log.close(); running.remove(entry)
                    status = "PASS" if rc == 0 else "SKIP" if rc == 77 else "FAIL"
                    report(name, status, time.monotonic() - started, reason or (f"exit={rc}" if rc else ""))
            if running:
                time.sleep(0.01)
    finally:
        if running:
            stop_processes([entry[1] for entry in running])
            for name, process, log, started, temporary in running:
                log.write(b"[ERROR] runner: interrupted; process group stopped\n")
                cleanup(name, temporary, log); log.close()
    print("\n==========================================")
    print(f"Test Results: {counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped ({len(args.tests)} total)")
    print(f"Log directory: {args.log_dir}")
    print("==========================================")
    if failed:
        print("Failed tests: " + " ".join(failed))
    return int(bool(failed))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=positive_int, default=os.environ.get("TEST_JOBS", str(available_cpus())))
    parser.add_argument("--timeout", type=positive_int, default=os.environ.get("TEST_TIMEOUT", "90"),
                        help="per-test timeout in seconds (default: TEST_TIMEOUT or 90)")
    parser.add_argument("--log-dir", type=Path, default=Path("logs"))
    parser.add_argument("tests", nargs="+")
    args = parser.parse_args()
    if len(set(args.tests)) != len(args.tests) or any(Path(name).name != name for name in args.tests):
        parser.error("test names must be unique basenames")
    requested_signals = []

    # Не выбрасываем исключение внутри Popen: сначала учитываем новый процесс и его fd.
    def interrupted(signum, frame):
        if not requested_signals:
            requested_signals.append(signum)

    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    try:
        args.log_dir.mkdir(parents=True, exist_ok=True)
        with lock_logs(args.log_dir):
            return run_tests(args, requested_signals)
    except InterruptedRun as error:
        print(f"[ERROR] runner interrupted by signal {error.signum}; test processes stopped", file=sys.stderr, flush=True)
        return 128 + error.signum
    except OSError as error:
        print(f"[ERROR] runner: {error}; log directory={args.log_dir} (check permissions or concurrent runs)", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
