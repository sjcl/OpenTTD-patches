"""Reject misleading admission evidence without running a game or using elapsed admission."""
import copy
import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("summary", pathlib.Path(__file__).with_name("Summarize.py"))
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.run = {"diagnostics": summary.profile.parse_console(
            "Periodic admission: accepted=1; rejected=0; reserved=65536; used=3; returned=65533; max_tick_queries=1; max_tick_work=3\n"
            "Periodic allocation: timed=49152; schedule=0; timed_used=3; schedule_used=0\n"
            "Graph work=0; query_allowance=3\n")}

    def test_valid_small_query(self):
        summary.check_accounting(self.run)

    def test_refund_leak(self):
        self.run["diagnostics"]["records"]["Periodic admission"]["returned"] -= 1
        with self.assertRaisesRegex(ValueError, "settle"):
            summary.check_accounting(self.run)

    def test_double_charge(self):
        self.run["diagnostics"]["records"]["Periodic allocation"]["schedule_used"] = 3
        with self.assertRaisesRegex(ValueError, "Planner work"):
            summary.check_accounting(self.run)

    def test_tick_budget_overrun(self):
        self.run["diagnostics"]["records"]["Periodic admission"]["max_tick_work"] = 1048577
        with self.assertRaisesRegex(ValueError, "Global tick Work"):
            summary.check_accounting(self.run)

    def test_old_reservation_misreported_as_actual(self):
        self.run["diagnostics"]["records"]["Graph work"]["query_allowance"] = 65536
        with self.assertRaisesRegex(ValueError, "actual work"):
            summary.check_accounting(self.run)


if __name__ == "__main__":
    unittest.main()
