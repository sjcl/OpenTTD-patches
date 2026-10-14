"""Check observation parsing and the distinction between a route and an optimum."""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
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
    def test_replacement_tus_keep_cmake_object_library_link_names(self):
        for script in ("Prepare-Legacy.py", "Prepare-Quality.py"):
            with self.subTest(script=script), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                source, build = root / "source", root / "build"
                (source / "src").mkdir(parents=True)
                (build / "journey-profile").mkdir(parents=True)
                original = source / "src/passenger_service_database.cpp"
                original.write_text("return this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true);\n"
                                    "\t\treturn result;\n\t};\n\tif (cargo >= NUM_CARGO", encoding="utf-8")
                project = build / "openttd_lib.vcxproj"
                project.write_text(str(original), encoding="utf-8")
                (build / "journey-profile/instrumentation.json").write_text("{}", encoding="utf-8")
                result = subprocess.run([sys.executable, "-B", str(HERE / script), str(source), str(build)],
                                        check=True, capture_output=True, text=True)
                replacement = pathlib.Path(result.stdout.strip())
                self.assertEqual(replacement.name, original.name)
                self.assertIn(str(replacement), project.read_text(encoding="utf-8-sig"))
                self.assertTrue(replacement.is_file())

    def test_work_buckets_are_not_counted_twice(self):
        buckets = "\n".join(f"Timed Work histogram: upper={i * 1024}; count={3 if i == 64 else 0}" for i in range(65))
        text = (buckets + "\nTimed Work completion: with_complete=2; without_complete=1; with_queued=2; with_popped=0; improvement_possible=2; reverse_work=90000; forward_work=106608\n"
                "Timed Work sample 0: complete=1; queued=1; popped=0; cost=50; arrival=64050; lower=20; reverse=32768; forward=32768; first_stop=Queue\n" + buckets)
        result = observations.completion(text)
        self.assertEqual(sum(b["count"] for b in result["work_histogram"]), 3)
        self.assertEqual(result["work_p95_upper"], 65536)
        self.assertEqual(result["rescuable_fraction"], 2 / 3)
        self.assertEqual(result["samples"][0]["first_stop"], "Queue")

    def completion_run(self):
        return {"diagnostics": {"periodic_outcomes": {"Work": {"outcomes": 2}, "Found": {"outcomes": 1}},
                                "records": {"Periodic database": {"searches": 3}}},
                "anytime": {"with_complete": 1, "without_complete": 1, "work_histogram": [{"count": 3}],
                            "results": {"unavailable": 0, "certified": 1, "provisional": 1, "deferred": 1,
                                        "no_route": 0, "rescued_unpopped": 1}}}

    def test_provisional_must_not_enter_found(self):
        run = self.completion_run()
        observations.validate_completion(run)
        run["diagnostics"]["periodic_outcomes"]["Found"]["outcomes"] = 2
        with self.assertRaises(ValueError):
            observations.validate_completion(run)

    def test_incomplete_state_or_work_populations_are_rejected(self):
        for field in ("work_histogram", "results"):
            run = self.completion_run()
            if field == "results":
                run["anytime"][field]["deferred"] = 0
            else:
                run["anytime"][field][0]["count"] = 2
            with self.assertRaises(ValueError):
                observations.validate_completion(run)

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
