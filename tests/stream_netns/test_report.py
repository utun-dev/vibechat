"""Агрегация CPU учитывает длительность интервала и границы фаз."""

import unittest

from report import cpu_by_phase, interface_bytes


def sample(phase, time, cpu):
    return {"phase": phase, "nodes": {"n01": {"sample_ns": time * 1000000000, "cpu_ns": cpu * 1000000000}}}


class ReportTest(unittest.TestCase):
    def test_weighted_cpu_and_phase_boundary(self):
        measured = cpu_by_phase([sample("baseline", 1, 0), sample("baseline", 2, 0.5),
                                 sample("baseline", 5, 0.8), sample("fault", 10, 4.8), sample("fault", 12, 5)])
        self.assertEqual(measured["baseline"]["n01"], {"core_percent": 20, "sampled_seconds": 4})
        self.assertEqual(measured["fault"]["n01"], {"core_percent": 10, "sampled_seconds": 2})

    def test_counter_reset_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "non-monotonic"):
            cpu_by_phase([sample("baseline", 1, 3), sample("baseline", 2, 1)])

    def test_single_sample_has_no_measured_interval(self):
        self.assertEqual(cpu_by_phase([sample("baseline", 1, 3)]), {})

    def test_ingress_ifb_not_counted_twice(self):
        links = [{"ifname": name, "stats64": {"tx": {"bytes": tx}, "rx": {"bytes": rx}}}
                 for name, tx, rx in [("lo", 3, 3), ("ifb0", 7, 9), ("eth0", 11, 13)]]
        self.assertEqual(interface_bytes({"n01": {"links": links}}, "n01"), {"tx": 11, "rx": 13})


if __name__ == "__main__":
    unittest.main()
