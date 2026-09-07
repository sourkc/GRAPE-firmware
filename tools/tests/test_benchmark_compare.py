import contextlib
import csv
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
import zlib

spec = importlib.util.spec_from_file_location("benchmark_compare", Path(__file__).parents[1] / "benchmark_compare.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def write_csv(path, records):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)


class CompareTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.a = Path(self.temp.name) / "baseline"
        self.b = Path(self.temp.name) / "candidate"
        self.make_run(self.a, 100)
        self.make_run(self.b, 50)

    def make_run(self, folder, timing):
        folder.mkdir()
        (folder / "grape_benchmark_metadata.txt").write_text(
            "".join(f"{key}=0\n" for key in module.MATCH_SETTINGS) +
            "schema_version=5\nbenchmark_build=baseline-v5\nprofile=1\n"
            "reference_frames=1\nreferences_complete=0\nreferences_complete=1\n"
            "gpu_stats=0\noptimization_config=debug_Og\n")
        row = dict(status="ok", kind="scene", group="baseline2d", name="rgb565_copy",
                   iterations="128", work_mean_us=str(timing), present_mean_us="0",
                   total_mean_us=str(timing))
        for i in range(6):
            row[f"param{i}_name"] = "width" if i == 0 else ""
            row[f"param{i}_value"] = "2" if i == 0 else "0"
        write_csv(folder / "grape_benchmark_summary.csv", [row])
        self.raw(folder, b"\x00\xf8\xe0\x07")

    def raw(self, folder, data):
        (folder / "frame.raw").write_bytes(data)
        row = dict(group="baseline2d", name="rgb565_copy", frame="0", plane="display",
                   format="rgb565le", width="2", height="1", samples="1", bytes=str(len(data)),
                   crc32=f"{zlib.crc32(data):08x}", file="frame.raw")
        write_csv(folder / "grape_benchmark_references.csv", [row])

    def test_case_selection_mismatch(self):
        p = self.b / "grape_benchmark_metadata.txt"
        p.write_text(p.read_text() + "case_selection=regressions-v1\n")
        self.assertIn("case_selection", str(module.compare(self.a, self.b)["settings_mismatch"]))

    def test_legacy_selection_defaults_to_all(self):
        for folder in (self.a, self.b):
            p = folder / "grape_benchmark_metadata.txt"
            p.write_text("\n".join(line for line in p.read_text().splitlines()
                                   if not line.startswith("case_selection=")) + "\n")
        self.assertEqual(module.metadata(self.a)["case_selection"], "all")
        self.assertEqual(module.compare(self.a, self.b)["settings_mismatch"], [])

    def test_speedup_and_exact_reference(self):
        result = module.compare(self.a, self.b)
        self.assertEqual(result["timings"][0]["total"]["speedup"], 2)
        self.assertEqual(result["changed_references"], [])
        self.assertEqual(result["reference_problems"], [])

    def test_intended_optimization_change_allowed(self):
        p = self.b / "grape_benchmark_metadata.txt"
        p.write_text(p.read_text().replace("debug_Og", "performance_O2"))
        self.assertEqual(module.compare(self.a, self.b)["settings_mismatch"], [])

    def test_diagnostic_build_mismatch(self):
        p = self.b / "grape_benchmark_metadata.txt"
        p.write_text(p.read_text().replace("gpu_stats=0", "gpu_stats=1"))
        self.assertEqual(module.compare(self.a, self.b)["settings_mismatch"][0]["setting"], "gpu_stats")

    def test_missing_build_metadata_rejected(self):
        p = self.b / "grape_benchmark_metadata.txt"
        p.write_text(p.read_text().replace("cpu_frequency_mhz=0\n", ""))
        with self.assertRaisesRegex(ValueError, "missing build metadata"):
            module.compare(self.a, self.b)

    def test_changed_pixels_with_valid_crc(self):
        self.raw(self.b, b"\x01\xf8\xe0\x07")
        self.assertEqual(module.compare(self.a, self.b)["changed_references"][0]["changed_bytes"], 1)

    def test_corrupt_capture_rejected(self):
        (self.b / "frame.raw").write_bytes(b"\x01\xf8\xe0\x07")
        with self.assertRaisesRegex(ValueError, "CRC mismatch"):
            module.compare(self.a, self.b)

    def test_missing_raw_file_rejected(self):
        (self.b / "frame.raw").unlink()
        with self.assertRaises(FileNotFoundError): module.compare(self.a, self.b)

    def test_truncated_file_rejected(self):
        (self.b / "frame.raw").write_bytes(b"\x00")
        with self.assertRaisesRegex(ValueError, "byte count"):
            module.compare(self.a, self.b)

    def test_incomplete_replay_rejected(self):
        p = self.b / "grape_benchmark_metadata.txt"
        p.write_text(p.read_text() + "references_complete=0\n")
        self.assertTrue(module.compare(self.a, self.b)["reference_problems"])

    def test_missing_reference_even_on_both_sides(self):
        for folder in (self.a, self.b):
            p = folder / "grape_benchmark_metadata.txt"
            p.write_text(p.read_text().replace("reference_frames=1", "reference_frames=2"))
        self.assertTrue(module.compare(self.a, self.b)["reference_problems"])

    def test_missing_case_detected(self):
        p = self.b / "grape_benchmark_summary.csv"
        records = module.rows(self.b, p.name)
        second = dict(records[0], name="additional_case")
        write_csv(p, records + [second])
        self.assertTrue(module.compare(self.a, self.b)["missing_cases"])

    def test_duplicate_case_rejected(self):
        p = self.b / "grape_benchmark_summary.csv"
        records = module.rows(self.b, p.name)
        write_csv(p, records + records)
        with self.assertRaisesRegex(ValueError, "Duplicate"): module.compare(self.a, self.b)

    def test_regression_flag(self):
        p = self.b / "grape_benchmark_summary.csv"
        records = module.rows(self.b, p.name)
        records[0]["work_mean_us"] = "110"
        records[0]["total_mean_us"] = "110"
        write_csv(p, records)
        self.assertTrue(module.compare(self.a, self.b)["timings"][0]["regression"])

    def test_path_escape_rejected(self):
        p = self.b / "grape_benchmark_references.csv"
        records = module.rows(self.b, p.name)
        records[0]["file"] = "../outside.raw"
        write_csv(p, records)
        with self.assertRaisesRegex(ValueError, "escapes"): module.compare(self.a, self.b)

    def test_cli_json_and_exit_code(self):
        output = Path(self.temp.name) / "comparison.json"
        with contextlib.redirect_stdout(io.StringIO()):
            result = module.main([str(self.a), str(self.b), "--output", str(output)])
        self.assertEqual(result, 0)
        self.assertTrue(output.is_file())


if __name__ == "__main__": unittest.main()
