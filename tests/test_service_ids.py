#!/usr/bin/env python3
"""Проверка общего реестра ETCP: значения, единственное объявление и таблица регистрации."""

import os
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = Path("src/transport_layer/etcp_api.h")
MATRIX = Path("tests/fixtures/group_isolation_matrix.txt")
ID = r"ETCP_(?:RT_)?ID_[A-Z0-9_]+"
DEFINE = re.compile(r"^\s*#\s*define\s+(" + ID + r")\s+([^\n]+)", re.MULTILINE)
COMMENTS = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*', re.DOTALL)
BIND = re.compile(r"\b(etcp_bind|etcp_unbind|merkle_sync_init|etcp_router_bind|etcp_router_unbind|"
                  r"etcp_router_require_crypto|etcp_router_set_receive_ready)\s*\([^,]+,\s*(" + ID + r"|0x[\da-fA-F]+|\d+)\b")
CALLBACK = re.compile(r"\b(api_bindings|router_bindings)\.callbacks\s*\[\s*(" + ID + r"|0x[\da-fA-F]+|\d+)\s*\]")


def code(text):
    return COMMENTS.sub(lambda m: m[0] if m[0][0] in "\"'" else "\n" * m[0].count("\n"), text)


def registry(text):
    ids, values, errors = {}, {}, []
    for match in DEFINE.finditer(code(text)):
        name, literal = match.groups()
        if name in ids:
            errors.append(f"{name}: repeated declaration")
        try:
            value = int(literal.strip(), 0)
        except ValueError:
            errors.append(f"{name}: expected a numeric literal, got {literal!r}")
            continue
        if not 0 <= value <= 255:
            errors.append(f"{name}: ID {value} does not fit in one byte")
        space = "router" if name.startswith("ETCP_RT_ID_") else "raw"
        previous = values.get((space, value))
        if previous:
            errors.append(f"{space} ID {value:#04x}: {previous} conflicts with {name}")
        ids[name] = value
        values[space, value] = name
    return ids, errors


def check_source(text, ids):
    text = code(text)
    errors = [f"{name}: declare only in {HEADER}" for name, _ in DEFINE.findall(text)]
    errors += [f"{name}: missing from {HEADER}" for name in sorted(set(re.findall(ID, text)) - ids.keys())]
    uses = [(m[1], m[2], m[1].startswith("etcp_router_")) for m in BIND.finditer(text)]
    uses += [(m[1], m[2], m[1] == "router_bindings") for m in CALLBACK.finditer(text)]
    for operation, name, routed in uses:
        prefix = "ETCP_RT_ID_" if routed else "ETCP_ID_"
        if not name.startswith(prefix):
            errors.append(f"{operation}: {name} must use a declared {prefix} identifier")
    return errors


def check_matrix(text, ids):
    """У каждого ID ровно один контракт и существующие тесты либо согласованное исключение UTUN."""
    rows, errors = set(), []
    utun = {"ETCP_ID_DATA", "ETCP_RT_ID_DATA", "ETCP_RT_ID_NAT", "ETCP_RT_ID_TCP_PROXY_SERVER",
            "ETCP_RT_ID_UDP_PROXY", "ETCP_RT_ID_ICMP_PROXY", "ETCP_RT_ID_TCP_PROXY_CLIENT"}
    scopes = {"GROUP", "PERSONAL", "TRANSPORT", "OPT_IN", "RESERVED", "UTUN"}
    for line in text.splitlines():
        if not line.startswith("ETCP_"):
            continue
        fields = [value.strip() for value in line.split("|")]
        if len(fields) != 4:
            errors.append(f"matrix: expected four columns: {line}")
            continue
        name, scope, boundary, tests = fields
        if name not in ids:
            errors.append(f"matrix: unknown ID {name}")
        if name in rows:
            errors.append(f"matrix: repeated ID {name}")
        rows.add(name)
        if scope not in scopes or not boundary:
            errors.append(f"matrix: missing/invalid scope or boundary for {name}")
        if scope == "UTUN":
            if name not in utun or tests != "-":
                errors.append(f"matrix: unapproved UTUN exclusion for {name}")
        elif tests == "-" or not tests:
            errors.append(f"matrix: no regression sources for {name}")
        else:
            for test in tests.split(";"):
                path = Path(test.strip())
                if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] not in {"tests", "tools"}:
                    errors.append(f"matrix: invalid test path for {name}: {test}")
                elif not (ROOT / path).is_file():
                    errors.append(f"matrix: missing test for {name}: {test}")
    errors += [f"matrix: missing ID {name}" for name in sorted(ids.keys() - rows)]
    return errors


def sources():
    excluded = {"tdesktop-dev", "third_party", "zxing-cpp", "openssl", "libopus", "speexdsp", "SoundTouch"}
    artifacts = {ROOT / "tools/etcp_lab" / name for name in ("runs", "archives")}
    for tree in ("src", "lib", "tools"):
        for directory, children, files in os.walk(ROOT / tree):
            children[:] = sorted(name for name in children if not name.startswith((".", "build")) and name not in excluded
                                 and Path(directory) / name not in artifacts)
            for name in sorted(files):
                path = Path(directory) / name
                if path.suffix in {".c", ".h", ".cpp", ".hpp", ".cc", ".cxx"} and path != ROOT / HEADER and path.is_file():
                    yield path


class ServiceIdsTests(unittest.TestCase):
    def test_matrix_guards(self):
        ids = {"ETCP_ID_SAMPLE": 1}
        valid = "ETCP_ID_SAMPLE | TRANSPORT | peer identity | tests/test_ncd_faults.c"
        self.assertEqual(check_matrix(valid, ids), [])
        for text in ("", valid + "\n" + valid, valid.replace("SAMPLE", "UNKNOWN"),
                     valid.replace("TRANSPORT", "UNKNOWN"), valid.replace("peer identity", ""),
                     valid.replace("tests/test_ncd_faults.c", "tests/no_such_regression.c"),
                     valid.replace("tests/test_ncd_faults.c", "-"), valid.replace("TRANSPORT", "UTUN")):
            self.assertTrue(check_matrix(text, ids), text)

    def test_separate_tables(self):
        ids, errors = registry("#define ETCP_ID_A 1\n#define ETCP_RT_ID_B 1\n")
        self.assertEqual(errors, [])
        self.assertEqual(len(ids), 2)

    def test_conflicting_values_and_names(self):
        for text in ("#define ETCP_RT_ID_A 0x40\n#define ETCP_RT_ID_B 0x40\n",
                     "#define ETCP_ID_A 1\n#define ETCP_ID_A 2\n"):
            self.assertTrue(registry(text)[1])

    def test_range_and_alias(self):
        for literal in ("-1", "256", "ETCP_ID_OTHER"):
            self.assertTrue(registry(f"#define ETCP_ID_A {literal}\n")[1])

    def test_wrong_binding_and_missing_declaration(self):
        ids = {"ETCP_ID_A": 1, "ETCP_RT_ID_B": 2}
        for text in ("etcp_bind(inst, ETCP_RT_ID_B, cb);", "etcp_router_bind(inst, ETCP_ID_A, cb);",
                     "inst->api_bindings.callbacks[ETCP_RT_ID_B](conn, packet);", "etcp_bind(inst, 7, cb);",
                     "inst->router_bindings.callbacks[0](conn, packet);",
                     "packet->cmd = ETCP_ID_MISSING;", "#define ETCP_ID_A 1\n"):
            self.assertTrue(check_source(text, ids), text)
        self.assertFalse(check_source("etcp_bind(inst, ETCP_ID_A, cb); etcp_router_bind(inst, ETCP_RT_ID_B, cb);", ids))

    def test_repository(self):
        ids, errors = registry((ROOT / HEADER).read_text())
        self.assertTrue(ids, f"empty registry in {HEADER}")
        errors += check_matrix((ROOT / MATRIX).read_text(), ids)
        checked = 0
        for path in sources():
            errors += [f"{path.relative_to(ROOT)}: {error}" for error in check_source(path.read_text(), ids)]
            checked += 1
        self.assertEqual(errors, [], "\n" + "\n".join(errors))
        raw = sum(not name.startswith("ETCP_RT_ID_") for name in ids)
        print(f"[INFO] service ID audit passed: raw={raw} router={len(ids) - raw} source_files={checked}", flush=True)


if __name__ == "__main__":
    unittest.main()
