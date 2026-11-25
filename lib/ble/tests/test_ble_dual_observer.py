"""Проверки доказательств пробы: не выдавать старое имя или один Started/echo за совместный эфир."""
import unittest
from ble_dual_observer import Evidence, NAME, SERVER_ID, VERSION


def packet(node=SERVER_ID, name=NAME):
    return b"VC" + bytes([VERSION]) + node.to_bytes(8, "big") + name.encode()


class ObserverTest(unittest.TestCase):
    def connected(self):
        result = Evidence()
        result.peer_line("DUAL_ENDPOINT node=test", 0)
        result.peer_line("DUAL_LINK_UP node=test", 1)
        for i in range(10):
            result.peer_line("DUAL_ECHO sequence=test", i + 2)
        result.peer_line("DUAL_PEER_PASS verified=10", 32)
        return result

    def test_requires_external_name_evidence(self):
        evidence = self.connected()
        self.assertFalse(evidence.result(0)["ok"])
        evidence.advertisement(packet(), "beacon-address", 0)
        self.assertFalse(evidence.result(0)["ok"])

    def test_rejects_wrong_identity_and_accepts_other_beacon_address(self):
        evidence = self.connected()
        self.assertFalse(evidence.advertisement(packet(node=1), "other", 2))
        self.assertFalse(evidence.advertisement(packet(name="other"), "other", 2))
        self.assertFalse(evidence.advertisement(b"VC", "other", 2))
        for now in (2, 10, 18, 26, 31):
            self.assertTrue(evidence.advertisement(packet(), "beacon-address", now))
        self.assertTrue(evidence.result(0)["ok"])
        self.assertFalse(evidence.result(1)["ok"])

    def test_silent_gap_or_no_gatt_is_not_success(self):
        evidence = self.connected()
        for now in (2, 3, 4, 31):
            evidence.advertisement(packet(), "beacon-address", now)
        self.assertFalse(evidence.result(0)["ok"])
        names_only = Evidence()
        for now in range(40):
            names_only.advertisement(packet(), "beacon-address", now)
        self.assertFalse(names_only.result(0)["ok"])


if __name__ == "__main__":
    unittest.main()
