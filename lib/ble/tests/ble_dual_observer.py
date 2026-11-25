#!/usr/bin/python3
"""Linux observer: только живые BlueZ manufacturer-события + отдельный GATT echo-клиент.

Запуск: /usr/bin/python3 ble_dual_observer.py --peer /path/to/ble_dual_peer
Зависимости: python3-dbus, python3-gi. Windows-проба уже должна работать.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

SERVER_ID = 0x56434455414C0001
NAME = "VC-dual-probe"
VERSION = 5
MAX_GAP = 10.0


class Evidence:
    """Отдельные часы VC-маяка и GATT: кеш/другой ID/неправильное имя не подтверждают эфир."""

    def __init__(self):
        self.names = []
        self.addresses = set()
        self.link_up = None
        self.peer_pass = None
        self.echoes = 0
        self.endpoint = False

    def advertisement(self, data, address, now):
        if len(data) < 11 or len(data) > 24 or data[:3] != b"VC" + bytes([VERSION]):
            return False
        if int.from_bytes(data[3:11], "big") != SERVER_ID or data[11:] != NAME.encode():
            return False
        self.names.append(now)
        self.addresses.add(address)
        return True

    def peer_line(self, line, now):
        if "DUAL_ENDPOINT " in line:
            self.endpoint = True
        if "DUAL_LINK_UP " in line:
            self.link_up = now
        if "DUAL_ECHO " in line:
            self.echoes += 1
        if "DUAL_PEER_PASS " in line:
            self.peer_pass = now

    def result(self, returncode):
        during = [] if self.link_up is None or self.peer_pass is None else [
            t for t in self.names if self.link_up <= t <= self.peer_pass
        ]
        gaps = []
        if self.link_up is not None and self.peer_pass is not None:
            checkpoints = [self.link_up] + during + [self.peer_pass]
            gaps = [b - a for a, b in zip(checkpoints, checkpoints[1:])]
        max_gap = max(gaps, default=float("inf"))
        held = 0 if self.link_up is None or self.peer_pass is None else self.peer_pass - self.link_up
        ok = (returncode == 0 and self.endpoint and self.echoes >= 10 and held >= 29.5
              and len(during) >= 3 and max_gap <= MAX_GAP)
        return {"ok": ok, "peer_exit": returncode, "service_endpoint_seen": self.endpoint,
                "echoes": self.echoes, "connected_seconds": round(held, 3), "name_events": len(self.names),
                "name_events_during_connection": len(during),
                "max_name_gap_seconds": round(max_gap, 3) if gaps else None,
                "name_addresses": sorted(self.addresses)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--peer", required=True, type=Path)
    parser.add_argument("--seconds", type=int, default=120)
    parser.add_argument("--log", type=Path, default=Path("ble_dual_observer.log"))
    args = parser.parse_args()
    if not args.peer.is_file() or not 45 <= args.seconds <= 600:
        parser.error("--peer должен указывать на собранный ble_dual_peer; --seconds: 45..600")

    import dbus
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib

    evidence = Evidence()
    started = time.monotonic()
    output = args.log.open("w", encoding="utf-8")

    def log(message):
        text = f"{time.monotonic() - started:8.3f} {message}"
        print(text, flush=True)
        output.write(text + "\n")
        output.flush()

    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    def properties(interface, changed, invalidated, path=None):
        if str(interface) != "org.bluez.Device1":
            return
        manufacturer = changed.get("ManufacturerData", {})
        for company, data in manufacturer.items():
            if int(company) != 0xFFFF:
                continue
            now = time.monotonic()
            if evidence.advertisement(bytes(data), str(path), now):
                log(f"VC_NAME node={SERVER_ID:016x} name={NAME} endpoint={path} "
                    f"rssi={changed.get('RSSI', 'unknown')} count={len(evidence.names)} source=live_signal")

    def added(path, interfaces):
        properties("org.bluez.Device1", interfaces.get("org.bluez.Device1", {}), [], path)

    # Никакого GetManagedObjects: начальные кешированные данные не участвуют в доказательстве.
    bus.add_signal_receiver(properties, signal_name="PropertiesChanged", dbus_interface="org.freedesktop.DBus.Properties",
                            bus_name="org.bluez", path_keyword="path")
    bus.add_signal_receiver(added, signal_name="InterfacesAdded", dbus_interface="org.freedesktop.DBus.ObjectManager",
                            bus_name="org.bluez")
    loop = GLib.MainLoop()
    process = None
    io_errors = []
    try:
        log(f"START expected_node={SERVER_ID:016x} expected_name={NAME} max_name_gap={MAX_GAP}s")
        process = subprocess.Popen([str(args.peer.resolve()), str(args.seconds)], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        os.set_blocking(process.stdout.fileno(), False)
        buffered = b""

        def drain_peer():
            nonlocal buffered
            while True:
                try:
                    data = os.read(process.stdout.fileno(), 65536)
                except BlockingIOError:
                    return True
                if not data:
                    if buffered:
                        log("PEER " + buffered.decode("utf-8", errors="replace"))
                        buffered = b""
                    return False
                buffered += data
                while b"\n" in buffered:
                    line, buffered = buffered.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace").rstrip()
                    evidence.peer_line(text, time.monotonic())
                    log("PEER " + text)

        def peer_output(stream, condition):
            try:
                return drain_peer()
            except Exception as error:
                io_errors.append(str(error))
                log(f"ERROR peer output: {error}")
                loop.quit()
                return False

        GLib.io_add_watch(process.stdout, GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, peer_output)

        def finished():
            if process.poll() is None:
                return True
            drain_peer()
            loop.quit()
            return False

        GLib.timeout_add(100, finished)

        def timeout():
            io_errors.append("observer deadline")
            log("ERROR observer deadline")
            loop.quit()
            return False

        GLib.timeout_add_seconds(args.seconds + 10, timeout)
        loop.run()
    except KeyboardInterrupt:
        io_errors.append("interrupted")
        log("ERROR interrupted")
    except Exception as error:
        io_errors.append(str(error))
        log(f"ERROR {error}")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                log("ERROR peer did not stop; killing probe child")
                process.kill()
                process.wait()
    result = evidence.result(process.returncode if process is not None else -1)
    result["errors"] = io_errors
    result["ok"] = result["ok"] and not io_errors
    log(("DUAL_OBSERVER_PASS " if result["ok"] else "DUAL_OBSERVER_FAIL ") + json.dumps(result, ensure_ascii=False))
    output.close()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
