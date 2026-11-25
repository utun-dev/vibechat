"""Непривилегированные проверки границы сетевых операций стенда."""

import tempfile
import unittest
from unittest.mock import Mock, patch

from network import Network
from run import stable_snapshot


class IsolationTest(unittest.TestCase):
    def test_refuse_main_namespace(self):
        with tempfile.TemporaryDirectory() as directory, patch("network.os.readlink", return_value="net:[1]"):
            with self.assertRaisesRegex(RuntimeError, "new network namespace"):
                Network(directory, 1, "net:[1]")

    def test_lost_namespace_never_executes_command(self):
        with tempfile.TemporaryDirectory() as directory, patch("network.os.readlink", return_value="net:[2]"):
            network = Network(directory, 1, "net:[1]")
            keeper = Mock(pid=42)
            keeper.poll.return_value = 0
            network.namespaces["n01"] = (keeper, "net:[3]")
            with patch("network.subprocess.run") as command:
                with self.assertRaisesRegex(RuntimeError, "no longer owned"):
                    network.command("n01", ["ip", "link", "add", "x", "type", "dummy"])
                command.assert_not_called()
            network.close()

    def test_controller_reentry_never_executes_command(self):
        with tempfile.TemporaryDirectory() as directory, patch("network.os.readlink", return_value="net:[2]"):
            network = Network(directory, 1, "net:[1]")
            with patch("network.os.readlink", return_value="net:[1]"), patch("network.subprocess.run") as command:
                with self.assertRaisesRegex(RuntimeError, "Controller changed"):
                    network.command(None, ["ip", "link", "add", "x", "type", "dummy"])
                command.assert_not_called()
            network.close()

    def test_namespace_inode_replacement_never_executes_command(self):
        with tempfile.TemporaryDirectory() as directory, patch("network.os.readlink", return_value="net:[2]"):
            network = Network(directory, 1, "net:[1]")
            keeper = Mock(pid=42)
            keeper.poll.return_value = None
            network.namespaces["n01"] = (keeper, "net:[3]")
            with patch("network.os.readlink", side_effect=["net:[2]", "net:[4]"]), patch("network.subprocess.run") as command:
                with self.assertRaisesRegex(RuntimeError, "no longer owned"):
                    network.command("n01", ["nft", "flush", "ruleset"])
                command.assert_not_called()
            network.close()

    def test_lifetime_ignored_but_route_change_detected(self):
        a = {"addr_info": [{"local": "192.0.2.1", "valid_life_time": 500}], "route": "192.0.2.0/24"}
        b = {"addr_info": [{"local": "192.0.2.1", "valid_life_time": 400}], "route": "192.0.2.0/24"}
        self.assertEqual(stable_snapshot(a), stable_snapshot(b))
        b["route"] = "0.0.0.0/0"
        self.assertNotEqual(stable_snapshot(a), stable_snapshot(b))


if __name__ == "__main__":
    unittest.main()
