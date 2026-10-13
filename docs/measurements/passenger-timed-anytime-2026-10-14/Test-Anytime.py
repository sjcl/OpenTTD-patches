"""Check observation parsing and the distinction between a route and an optimum."""
import importlib.util
import pathlib
import unittest

HERE = pathlib.Path(__file__).resolve().parent


def module(filename):
    spec = importlib.util.spec_from_file_location(filename, HERE / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


observations = module("Summarize-Anytime.py")
quality = module("Summarize-Quality.py")


class AnytimeEvidence(unittest.TestCase):
    def test_work_buckets_are_not_counted_twice(self):
        buckets = "\n".join(f"Timed Work histogram: upper={i * 1024}; count={3 if i == 64 else 0}" for i in range(65))
        text = (buckets + "\nTimed Work completion: with_complete=2; without_complete=1; with_queued=2; with_popped=0; improvement_possible=2; reverse_work=90000; forward_work=106608\n"
                "Timed Work sample 0: complete=1; queued=1; popped=0; cost=50; arrival=64050; lower=20; reverse=32768; forward=32768; first_stop=Queue\n" + buckets)
        result = observations.completion(text)
        self.assertEqual(sum(b["count"] for b in result["work_histogram"]), 3)
        self.assertEqual(result["work_p95_upper"], 65536)
        self.assertEqual(result["rescuable_fraction"], 2 / 3)
        self.assertEqual(result["samples"][0]["first_stop"], "Queue")

    def row(self, **changes):
        result = dict(work=65536, old_work=65536, found=1, old_found=0, state=2, old_deferred=1,
                      cost=30, arrival=64030, old_cost=0, old_arrival=0,
                      reference_found=1, reference_certified=1, reference_cost=20, reference_arrival=64020)
        result.update(changes)
        return result

    def test_uncertified_reference_is_excluded_from_optimum_deltas(self):
        result = quality.summarize([self.row(reference_certified=0, reference_cost=10)])
        self.assertEqual(result["uncertified_references"], 1)
        self.assertIsNone(result["reference_cost_difference"])
        self.assertEqual(result["rescued"], 1)

    def test_a_certified_reference_cannot_be_beaten_in_cost(self):
        with self.assertRaises(ValueError):
            quality.summarize([self.row(cost=10)])

    def test_same_budget_work_divergence_is_rejected(self):
        with self.assertRaises(ValueError):
            quality.summarize([self.row(old_work=65535)])

    def test_earlier_arrival_can_have_higher_generalized_cost(self):
        result = quality.summarize([self.row(arrival=64010)])
        self.assertEqual(result["reference_cost_difference"]["mean"], 10)
        self.assertEqual(result["reference_arrival_difference"]["mean"], -10)


if __name__ == "__main__":
    unittest.main()
