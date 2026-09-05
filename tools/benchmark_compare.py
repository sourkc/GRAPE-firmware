#!/usr/bin/env python3
"""Compare two baseline-v5 SD run folders. Python standard library only."""
import argparse
import csv
import json
import math
from pathlib import Path
import sys
import zlib

MATCH_SETTINGS = (
    "schema_version", "benchmark_build", "idf_version", "idf_target", "compiler",
    "chip_model", "chip_revision", "chip_cores", "cpu_frequency_mhz",
    "psram_total_bytes", "psram_frequency_mhz", "l2_cache_bytes",
    "display_name", "display_width", "display_height", "display_format",
    "telemetry_level", "function_profiling", "gpu_multicore_build", "gpu_stats",
    "profile", "warmup_iterations", "measured_iterations", "fixed_dt_us", "seed",
    "suite_mask", "reference_frames", "service_interval_us", "service_delay_ticks",
    "refresh_wait_available", "ppa_fill_available", "ppa_blend_available",
)


def metadata(folder):
    result = {}
    for line in (folder / "grape_benchmark_metadata.txt").read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key] = value  # completion is appended after the reference replay
    if result.get("schema_version") != "5":
        raise ValueError(f"{folder}: expected schema_version=5; older harness runs are not equivalent")
    required = set(MATCH_SETTINGS) - {"psram_frequency_mhz", "l2_cache_bytes"}
    missing = required - result.keys()
    if missing:
        raise ValueError(f"{folder}: missing build metadata: {', '.join(sorted(missing))}")
    return result


def rows(folder, name):
    with (folder / name).open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        result = list(reader)
    if any(None in row or None in row.values() for row in result):
        raise ValueError(f"{folder / name}: malformed CSV row")
    return result


def unique(records, key_fn):
    result = {}
    for row in records:
        key = key_fn(row)
        if key in result:
            raise ValueError(f"Duplicate record: {key}")
        result[key] = row
    return result


def case_key(row):
    params = tuple((row[f"param{i}_name"], row[f"param{i}_value"])
                   for i in range(6) if row.get(f"param{i}_name"))
    return row["kind"], row["group"], row["name"], params


def reference_key(row):
    return row["group"], row["name"], row["frame"], row["plane"]


def read_reference(folder, row):
    path = (folder / row["file"]).resolve()
    if not path.is_relative_to(folder.resolve()):
        raise ValueError(f"Reference path escapes run directory: {row['file']}")
    data = path.read_bytes()
    bpp = {"rgb565le": 2, "rgb888": 3, "rgba8888": 4, "d16le": 2}.get(row["format"])
    dimensions = [int(row[k]) for k in ("width", "height", "samples")]
    if not bpp or any(n <= 0 for n in dimensions):
        raise ValueError(f"{path}: invalid reference format/dimensions")
    expected = math.prod(dimensions) * bpp
    if len(data) != int(row["bytes"]) or len(data) != expected:
        raise ValueError(f"{path}: incorrect byte count")
    if f"{zlib.crc32(data):08x}" != row["crc32"].lower():
        raise ValueError(f"{path}: CRC mismatch; capture is incomplete or corrupt")
    return data


def compare(baseline, candidate, regression_percent=5.0):
    baseline, candidate = Path(baseline), Path(candidate)
    ma, mb = metadata(baseline), metadata(candidate)
    mismatch = [{"setting": k, "baseline": ma.get(k), "candidate": mb.get(k)}
                for k in MATCH_SETTINGS if ma.get(k) != mb.get(k)]
    a = unique(rows(baseline, "grape_benchmark_summary.csv"), case_key)
    b = unique(rows(candidate, "grape_benchmark_summary.csv"), case_key)
    if not a or not b:
        raise ValueError("Empty timing summary")
    missing = [repr(k) for k in a.keys() ^ b.keys()]
    timings, skipped = [], []
    for key in sorted(a.keys() & b.keys()):
        left, right = a[key], b[key]
        if left["status"] != "ok" or right["status"] != "ok":
            skipped.append({"case": key[1] + "/" + key[2],
                            "baseline": left["status"], "candidate": right["status"]})
            if left["status"] != right["status"]:
                missing.append(f"{key[1]}/{key[2]} changed status")
            continue
        if left["iterations"] != right["iterations"]:
            mismatch.append({"setting": key[1] + "/" + key[2] + ":iterations",
                             "baseline": left["iterations"], "candidate": right["iterations"]})
        entry = {"case": key[1] + "/" + key[2], "params": dict(key[3])}
        for field in ("work", "present", "total"):
            old, new = float(left[field + "_mean_us"]), float(right[field + "_mean_us"])
            if not math.isfinite(old) or not math.isfinite(new) or old < 0 or new < 0:
                raise ValueError("Invalid timing value")
            entry[field] = {"baseline_us": old, "candidate_us": new,
                            "speedup": old / new if new else None,
                            "change_percent": 100 * (new / old - 1) if old else None}
        entry["regression"] = any(
            entry[f]["change_percent"] is not None and
            entry[f]["change_percent"] > regression_percent for f in ("work", "total"))
        timings.append(entry)
    ra = unique(rows(baseline, "grape_benchmark_references.csv"), reference_key)
    rb = unique(rows(candidate, "grape_benchmark_references.csv"), reference_key)
    reference_problems = []
    if ma.get("references_complete") != "1" or mb.get("references_complete") != "1":
        reference_problems.append("Reference replay disabled or incomplete")
    if not ra or not rb:
        reference_problems.append("No reference images")
    reference_problems += [f"Missing reference: {k}" for k in sorted(ra.keys() ^ rb.keys())]
    changed, checked = [], 0
    for key in sorted(ra.keys() & rb.keys()):
        left, right = ra[key], rb[key]
        # Validate actual files even when CRC strings match.
        da, db = read_reference(baseline, left), read_reference(candidate, right)
        if any(left[k] != right[k] for k in ("format", "width", "height", "samples")):
            reference_problems.append(f"Incompatible dimensions/format: {key}")
        elif da != db:
            changed.append({"reference": list(key), "format": left["format"],
                            "changed_bytes": sum(x != y for x, y in zip(da, db)),
                            "bytes": len(da), "baseline_file": left["file"],
                            "candidate_file": right["file"]})
        checked += 1
    # A capture manifest must cover every successfully measured focused case.
    for label, summary, refs in (("baseline", a, ra), ("candidate", b, rb)):
        expected_frames = int((ma if label == "baseline" else mb)["reference_frames"])
        for key, row in summary.items():
            if row["status"] != "ok" or key[1] not in ("baseline2d", "gpu3d"):
                continue
            planes = ("color", "depth") if key[1] == "gpu3d" else ("display",)
            if key[2] == "cube_4x_present_2x": planes += ("display",)
            for frame in range(expected_frames):
                for plane in planes:
                    if (key[1], key[2], str(frame), plane) not in refs:
                        reference_problems.append(f"{label}: missing {key[1]}/{key[2]} frame {frame} {plane}")
    return {"baseline": str(baseline), "candidate": str(candidate),
            "builds": [{k: m.get(k) for k in ("run_label", "optimization_config", "elf_sha256")}
                       for m in (ma, mb)],
            "settings_mismatch": mismatch, "missing_cases": missing, "skipped_cases": skipped,
            "reference_problems": reference_problems, "references_checked": checked,
            "changed_references": changed, "timings": timings,
            "notes": ["Speedup is baseline/candidate; values above 1 are faster.",
                      "Work/total regressions are flags for investigation, not statistical proof.",
                      "Sample totals exclude harness service gaps and are not sustained display FPS.",
                      "Use long-profile repetitions for tail latency; inspect raw samples for outliers."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", type=Path, help="Write a detailed JSON comparison")
    parser.add_argument("--regression-percent", type=float, default=5.0)
    parser.add_argument("--allow-config-change", action="store_true",
                        help="Permit an intentional settings mismatch; still lists every mismatch")
    args = parser.parse_args(argv)
    try:
        if not math.isfinite(args.regression_percent) or args.regression_percent < 0:
            raise ValueError("Regression threshold must be finite and nonnegative")
        result = compare(args.baseline, args.candidate, args.regression_percent)
    except (OSError, ValueError, KeyError) as exc:
        print(f"Cannot compare: {exc}", file=sys.stderr)
        return 2
    if args.output:
        args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    for entry in result["timings"]:
        speed = entry["total"]["speedup"]
        label = f"{speed:.3f}x" if speed is not None else "n/a"
        print(f"{entry['case']:<44} total {label:>8}" + ("  REGRESSION" if entry["regression"] else ""))
    print(f"References: {result['references_checked']} checked, {len(result['changed_references'])} changed")
    for field in ("settings_mismatch", "missing_cases", "reference_problems", "changed_references"):
        for issue in result[field]: print(f"{field}: {issue}")
    if result["settings_mismatch"] and not args.allow_config_change:
        return 2
    if result["missing_cases"] or result["reference_problems"]:
        return 2
    if result["changed_references"] or any(e["regression"] for e in result["timings"]):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
