"""End-to-end tests for the capture comparator; fixtures need no image libraries."""

import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "compare_captures.py"


def paeth(a, b, c):
    p = a + b - c
    distances = (abs(p - a), abs(p - b), abs(p - c))
    return (a, b, c)[distances.index(min(distances))]


def write_png(path, width, height, pixels, channels=3, filter_type=0):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    rows = []
    previous = bytes(width * channels)
    for y in range(height):
        row = bytes(value for pixel in pixels[y * width:(y + 1) * width] for value in pixel)
        encoded = bytearray()
        for x, value in enumerate(row):
            left = row[x - channels] if x >= channels else 0
            above = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            predictors = (0, left, above, (left + above) // 2,
                          paeth(left, above, upper_left))
            encoded.append((value - predictors[filter_type]) & 255)
        rows.append(bytes([filter_type]) + encoded)
        previous = row
    header = struct.pack(">IIBBBBB", width, height, 8, 2 if channels == 3 else 6, 0, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                     + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


def write_bmp(path, width, height, pixels, channels=3, top_down=False, padding=0):
    row_bytes = width * channels
    stride = (row_bytes + 3) & ~3
    rows = []
    for y in (range(height) if top_down else range(height - 1, -1, -1)):
        row = bytearray()
        for pixel in pixels[y * width:(y + 1) * width]:
            row.extend((pixel[2], pixel[1], pixel[0]))
            if channels == 4:
                row.append(pixel[3])
        row.extend(bytes([padding]) * (stride - row_bytes))
        rows.append(row)
    data = b"".join(rows)
    signed_height = -height if top_down else height
    header = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    header += struct.pack("<IiiHHIIiiII", 40, width, signed_height, 1,
                          channels * 8, 0, len(data), 2835, 2835, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + data)


def fallback_pixels(width, height, phase):
    # Mirror PSFallbackMain's UV, smoothstep and colors, including pixel centers.
    def smooth_reversed(edge, value):
        t = max(0.0, min(1.0, (edge - value) / edge))
        return t * t * (3.0 - 2.0 * t)

    pixels = []
    for y in range(height):
        for x in range(width):
            px = ((x + 0.5) / width - 0.5) * (width / height) * 2
            py = ((y + 0.5) / height - 0.5) * 2
            radius = math.hypot(px, py)
            ring = smooth_reversed(0.06, abs(radius - (0.42 + 0.08 * math.sin(phase))))
            core = smooth_reversed(0.18, radius)
            base, cyan, amber = (0.03, 0.05, 0.08), (0.06, 0.55, 0.50), (0.75, 0.45, 0.12)
            pixels.append(tuple(round(min(1.0, (a + (b - a) * ring + c * core)) * 255)
                                for a, b, c in zip(base, cyan, amber)))
    return pixels


class CompareCapturesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ref = self.root / "ref"
        self.run = self.root / "run"
        self.ref.mkdir()
        self.run.mkdir()
        # Deliberately varied: identical frames must also pass the flat-frame check.
        self.pixels = [(10, 20, 30), (50, 60, 70), (90, 100, 110),
                       (130, 140, 150), (170, 180, 190), (210, 220, 230)]

    def compare(self, *args):
        return subprocess.run([sys.executable, str(TOOL), "--ref", str(self.ref),
                               "--run", str(self.run), *map(str, args)],
                              cwd=ROOT, text=True, capture_output=True)

    def png_pair(self, run_pixels=None, name="nested/frame.png"):
        write_png(self.ref / name, 3, 2, self.pixels)
        write_png(self.run / name, 3, 2, self.pixels if run_pixels is None else run_pixels)

    def assert_exit(self, result, code):
        self.assertEqual(result.returncode, code, result.stdout + "\n" + result.stderr)

    def test_identical_and_json(self):
        self.png_pair()
        output = self.root / "report.json"
        result = self.compare("--json", output)
        self.assert_exit(result, 0)
        self.assertIn("nested/frame.png", result.stdout)
        report = json.loads(output.read_text(encoding="utf-8"))
        entry = report["files"][0]
        self.assertEqual(entry["path"], "nested/frame.png")
        self.assertEqual(entry["status"], "PASS")
        self.assertEqual([entry[k] for k in ("max", "mean", "over8", "over64")],
                         [0, 0, 0, 0])

    def test_one_pixel_slightly_different(self):
        pixels = self.pixels.copy()
        pixels[0] = (12, 21, 29)
        self.png_pair(pixels)
        self.assert_exit(self.compare(), 0)

    def test_channel_metrics_and_optional_over8_limit(self):
        pixels = self.pixels.copy()
        pixels[0] = (20, 90, 30)  # Two changed channels: differences 10 and 70.
        self.png_pair(pixels)
        report_path = self.root / "report.json"
        result = self.compare("--max-mean", 10, "--max-over64", 1,
                              "--max-over8", 0.1, "--json", report_path)
        self.assert_exit(result, 1)
        entry = json.loads(report_path.read_text(encoding="utf-8"))["files"][0]
        self.assertEqual(entry["max"], 70)
        self.assertAlmostEqual(entry["mean"], 80 / 18)
        self.assertAlmostEqual(entry["over8"], 2 / 18)
        self.assertAlmostEqual(entry["over64"], 1 / 18)

    def test_large_shift_fails_and_writes_heatmap(self):
        pixels = [tuple(255 - c for c in pixel) for pixel in self.pixels]
        self.png_pair(pixels)
        result = self.compare("--heatmaps")
        self.assert_exit(result, 1)
        self.assertTrue((self.run / "_diff" / "nested" / "frame.png.png").exists())
        # A previous heatmap must not become an input on the next invocation.
        self.assert_exit(self.compare(), 1)

    def test_different_size_fails(self):
        self.png_pair()
        write_png(self.run / "nested/frame.png", 2, 2, self.pixels[:4])
        self.assert_exit(self.compare(), 1)

    def test_missing_on_either_side_fails(self):
        write_png(self.ref / "only-ref.png", 3, 2, self.pixels)
        write_png(self.run / "only-run.png", 3, 2, self.pixels)
        result = self.compare()
        self.assert_exit(result, 1)
        self.assertIn("only-ref.png", result.stdout)
        self.assertIn("only-run.png", result.stdout)

    def test_volatile_excuses_difference_but_missing_fails(self):
        self.png_pair([tuple(255 - c for c in pixel) for pixel in self.pixels])
        write_png(self.ref / "missing.png", 3, 2, self.pixels)
        baseline = self.root / "baseline.json"
        # Windows PowerShell 5.1 writes -Encoding UTF8 with a BOM.
        baseline.write_text(json.dumps({"files": {"nested/frame.png": {"stable": False},
                                                "missing.png": {"stable": False}}}),
                            encoding="utf-8-sig")
        report_path = self.root / "volatile.json"
        result = self.compare("--baseline", baseline, "--json", report_path)
        self.assert_exit(result, 1)
        self.assertIn("volatile", result.stdout.lower())
        report = json.loads(report_path.read_text(encoding="utf-8"))
        entries = {entry["path"]: entry for entry in report["files"]}
        self.assertEqual(entries["nested/frame.png"]["status"], "VOLATILE")
        self.assertEqual(entries["missing.png"]["status"], "FAIL")
        self.assertIn("missing", entries["missing.png"]["reason"].lower())

    def test_flat_black_run_fails_even_against_itself(self):
        black = [(0, 0, 0)] * 6
        self.png_pair(black)
        write_png(self.ref / "nested/frame.png", 3, 2, black)
        self.assert_exit(self.compare(), 1)

    def test_rgba_alpha_is_ignored(self):
        ref = [(*pixel, 0) for pixel in self.pixels]
        run = [(*pixel, 255) for pixel in self.pixels]
        write_png(self.ref / "alpha.png", 3, 2, ref, channels=4)
        write_png(self.run / "alpha.png", 3, 2, run, channels=4)
        self.assert_exit(self.compare(), 0)

    def test_all_png_filters_decode_to_same_pixels(self):
        for filter_type in range(5):
            with self.subTest(filter_type=filter_type):
                name = f"filter{filter_type}.png"
                write_png(self.ref / name, 3, 2, self.pixels)
                write_png(self.run / name, 3, 2, self.pixels, filter_type=filter_type)
        self.assert_exit(self.compare("--max-mean", 0, "--max-over8", 0,
                                      "--max-over64", 0), 0)

    def test_bmp_bottom_up_top_down_and_alpha(self):
        pixels = [(*pixel, 17) for pixel in self.pixels]
        write_bmp(self.ref / "frame.bmp", 3, 2, pixels, channels=4)
        write_bmp(self.run / "frame.bmp", 3, 2,
                  [(*pixel[:3], 230) for pixel in pixels], channels=4, top_down=True)
        write_bmp(self.ref / "rgb.bmp", 3, 2, self.pixels)
        write_bmp(self.run / "rgb.bmp", 3, 2, self.pixels, top_down=True, padding=0xCD)
        self.assert_exit(self.compare("--max-mean", 0), 0)

    def test_read_errors_and_unsupported_png_are_per_file_failures(self):
        self.png_pair()
        (self.run / "nested/frame.png").write_bytes(b"not a PNG")
        self.png_pair(name="valid.png")
        result = self.compare()
        self.assert_exit(result, 1)
        self.assertIn("invalid PNG signature", result.stdout)
        self.assertIn("PASS valid.png", result.stdout)

        # Valid PNG container, but its grayscale color type is unsupported.
        def chunk(kind, data):
            return (struct.pack(">I", len(data)) + kind + data
                    + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
        header = struct.pack(">IIBBBBB", 3, 2, 8, 0, 0, 0, 0)
        gray = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
        gray += chunk(b"IDAT", zlib.compress(b"\0\0\x10\x20\0\x30\x40\x50"))
        gray += chunk(b"IEND", b"")
        (self.run / "nested/frame.png").write_bytes(gray)
        result = self.compare()
        self.assert_exit(result, 1)
        self.assertIn("only non-interlaced", result.stdout)
        self.assertIn("PASS valid.png", result.stdout)

    def test_stable_baseline_completeness_and_only(self):
        self.png_pair(name="film/film_family_18s_existing.png")
        baseline = self.root / "baseline.json"
        baseline.write_text(json.dumps({"files": {
            "film/film_family_18s_existing.png": {"stable": True},
            "film/film_family_18s_missing.png": {"stable": True},
            "film/other_missing.png": {"stable": True},
            "film/volatile_missing.png": {"stable": False},
        }}), encoding="utf-8")
        report = self.root / "report.json"
        result = self.compare("--baseline", baseline, "--json", report)
        self.assert_exit(result, 1)
        entries = {e["path"]: e for e in json.loads(report.read_text(encoding="utf-8"))["files"]}
        self.assertEqual(entries["film/film_family_18s_missing.png"]["status"], "FAIL")
        self.assertEqual(entries["film/other_missing.png"]["status"], "FAIL")
        self.assertEqual(entries["film/volatile_missing.png"]["status"], "FAIL")
        self.assertIn("missing run", entries["film/volatile_missing.png"]["reason"])
        result = self.compare("--baseline", baseline, "--only", "film/film_family_18s_*",
                              "--json", report)
        self.assert_exit(result, 1)
        entries = {e["path"]: e for e in json.loads(report.read_text(encoding="utf-8"))["files"]}
        self.assertEqual(set(entries), {"film/film_family_18s_existing.png",
                                        "film/film_family_18s_missing.png"})
        self.assertEqual(entries["film/film_family_18s_missing.png"]["status"], "FAIL")
        result = self.compare("--baseline", baseline, "--only", "*existing.png")
        self.assert_exit(result, 0)

    def test_repeatable_only_matches_union(self):
        for name in ("one.png", "two.png", "three.png"):
            self.png_pair(name=name)
        result = self.compare("--only", "one*", "--only", "two*")
        self.assert_exit(result, 0)
        self.assertIn("one.png", result.stdout)
        self.assertIn("two.png", result.stdout)
        self.assertNotIn("three.png", result.stdout)

    def test_flat_reference_fails(self):
        self.png_pair()
        write_png(self.ref / "nested/frame.png", 3, 2, [(40, 40, 40)] * 6)
        result = self.compare("--max-mean", 255, "--max-over64", 1)
        self.assert_exit(result, 1)
        self.assertIn("reference", result.stdout.lower())
        self.assertIn("flat", result.stdout.lower())

    def test_mostly_black_run_with_bright_overlay_fails(self):
        # Two bright pixels among 200 exceed the 0.5% flatness allowance.
        normal = [self.pixels[i % len(self.pixels)] for i in range(200)]
        dark = [(0, 0, 0)] * 198 + [(255, 255, 255), (255, 255, 255)]
        write_png(self.ref / "dark.png", 20, 10, normal)
        write_png(self.run / "dark.png", 20, 10, dark)
        result = self.compare("--max-mean", 255, "--max-over64", 1)
        self.assert_exit(result, 1)
        self.assertIn("black", result.stdout.lower())
        self.assertNotIn("flat", result.stdout.lower())

    def test_reject_image_detects_forbidden_picture(self):
        self.png_pair()
        reject = self.root / "reject.png"
        write_png(reject, 3, 2, self.pixels)
        result = self.compare("--reject", reject, "--max-mean", 255, "--max-over64", 1)
        self.assert_exit(result, 1)
        self.assertIn("reject", result.stdout.lower())

    def test_map_uses_run_names_and_baseline_completeness(self):
        run_name = "film/frame_preferred.png"
        ref_name = "film/frame_raster.png"
        write_png(self.run / run_name, 3, 2, self.pixels)
        write_png(self.ref / ref_name, 3, 2, self.pixels)
        baseline = self.root / "baseline.json"
        baseline.write_text(json.dumps({"files": {run_name: {"stable": True}}}), encoding="utf-8")
        report = self.root / "report.json"
        result = self.compare("--map", "_preferred=_raster", "--baseline", baseline,
                              "--json", report)
        self.assert_exit(result, 0)
        entries = json.loads(report.read_text(encoding="utf-8"))["files"]
        self.assertEqual([entry["path"] for entry in entries], [run_name])
        (self.run / run_name).unlink()
        result = self.compare("--map", "_preferred=_raster", "--baseline", baseline,
                              "--only", "film/*_preferred.png", "--json", report)
        self.assert_exit(result, 1)
        entries = json.loads(report.read_text(encoding="utf-8"))["files"]
        self.assertEqual(entries[0]["path"], run_name)
        self.assertEqual(entries[0]["status"], "FAIL")

    def test_mapped_missing_volatile_is_reported(self):
        preferred, raster = "film/shot_preferred.png", "film/shot_raster.png"
        write_png(self.ref / raster, 3, 2, self.pixels)
        write_png(self.run / raster, 3, 2, self.pixels)
        baseline = self.root / "baseline.json"
        baseline.write_text(json.dumps({"files": {preferred: {"stable": False},
                                                 raster: {"stable": True}}}), encoding="utf-8")
        report = self.root / "report.json"
        result = self.compare("--baseline", baseline, "--map", "_preferred=_raster", "--json", report)
        self.assert_exit(result, 1)
        entries = {e["path"]: e for e in json.loads(report.read_text(encoding="utf-8"))["files"]}
        self.assertEqual(entries[preferred]["status"], "FAIL")
        self.assertIn("missing run", entries[preferred]["reason"])
        self.assertEqual(entries[raster]["status"], "PASS")

    def test_same_resolved_folder_is_usage_error(self):
        result = subprocess.run([sys.executable, str(TOOL), "--ref", str(self.ref),
                                 "--run", str(self.ref / ".")], cwd=ROOT,
                                text=True, capture_output=True)
        self.assert_exit(result, 2)

    def test_volatile_structural_failures_remain_failures(self):
        baseline = self.root / "baseline.json"
        names = ["size.png", "flat.png", "bad.png", "bad_ref.png", "flat_ref.png"]
        baseline.write_text(json.dumps({"files": {name: {"stable": False} for name in names}}),
                            encoding="utf-8")
        for name in names:
            self.png_pair(name=name)
        write_png(self.run / "size.png", 2, 2, self.pixels[:4])
        write_png(self.run / "flat.png", 3, 2, [(0, 0, 0)] * 6)
        (self.run / "bad.png").write_bytes(b"bad PNG")
        (self.ref / "bad_ref.png").write_bytes(b"bad PNG")
        write_png(self.ref / "flat_ref.png", 3, 2, [(60, 60, 60)] * 6)
        report = self.root / "report.json"
        result = self.compare("--baseline", baseline, "--json", report)
        self.assert_exit(result, 1)
        entries = json.loads(report.read_text(encoding="utf-8"))["files"]
        self.assertEqual({e["path"]: e["status"] for e in entries},
                         {name: "FAIL" for name in names})

    def test_failure_list_sorts_worst_mean_first(self):
        small = [tuple(min(255, c + 2) for c in pixel) for pixel in self.pixels]
        large = [tuple(255 - c for c in pixel) for pixel in self.pixels]
        self.png_pair(small, name="a-small.png")
        self.png_pair(large, name="z-large.png")
        write_png(self.ref / "missing.png", 3, 2, self.pixels)
        result = self.compare("--max-mean", 0)
        self.assert_exit(result, 1)
        failures = result.stdout.split("Failures (worst mean first):", 1)[1].split("Summary:", 1)[0]
        self.assertLess(failures.index("z-large.png"), failures.index("a-small.png"))
        self.assertLess(failures.index("a-small.png"), failures.index("missing.png"))

    def test_heatmaps_remove_old_diff_folder(self):
        self.png_pair([tuple(255 - c for c in pixel) for pixel in self.pixels])
        stale = self.run / "_diff" / "stale.png"
        stale.parent.mkdir(parents=True)
        stale.write_bytes(b"stale")
        result = self.compare("--heatmaps")
        self.assert_exit(result, 1)
        self.assertFalse(stale.exists())
        self.assertTrue((self.run / "_diff" / "nested" / "frame.png.png").exists())

    def test_paeth_upper_left_and_up_tie(self):
        # For the second pixel on row two, (left, up, upper-left) is
        # (0, 2, 1) in red (upper-left wins), and (0, 3, 1) in green
        # (up and upper-left tie, so up wins).
        self.assertEqual(paeth(0, 2, 1), 1)
        self.assertEqual(paeth(0, 3, 1), 3)
        pixels = [(1, 1, 1), (2, 3, 5), (90, 100, 110),
                  (0, 0, 0), (40, 50, 60), (170, 180, 190)]
        write_png(self.ref / "paeth.png", 3, 2, pixels)
        write_png(self.run / "paeth.png", 3, 2, pixels, filter_type=4)
        self.assert_exit(self.compare("--max-mean", 0, "--max-over8", 0,
                                      "--max-over64", 0), 0)

    def test_rgba_png_filters_one_through_four(self):
        pixels = [(*pixel, (i * 61) % 256) for i, pixel in enumerate(self.pixels)]
        for filter_type in range(1, 5):
            name = f"rgba{filter_type}.png"
            write_png(self.ref / name, 3, 2, pixels, channels=4)
            write_png(self.run / name, 3, 2, pixels, channels=4, filter_type=filter_type)
        self.assert_exit(self.compare("--max-mean", 0, "--max-over8", 0,
                                      "--max-over64", 0), 0)


    def test_only_empty_selection_and_windows_separator(self):
        self.png_pair(name="film/frame.png")
        result = self.compare("--only", "missing/*")
        self.assert_exit(result, 2)
        self.assertIn("no matching", result.stderr.lower())
        result = self.compare("--only", r"film\*.png")
        self.assert_exit(result, 0)
        self.assertIn("film/frame.png", result.stdout)

    def test_diff_folder_is_skipped_case_insensitively(self):
        self.png_pair()
        write_png(self.run / "_DIFF" / "old.png", 3, 2, self.pixels)
        self.assert_exit(self.compare(), 0)

    def test_paeth_left_up_and_left_upper_left_ties(self):
        # The second pixel on row two has a left/up tie in red and
        # a left/upper-left distance tie in green, both resolved to left.
        self.assertEqual(paeth(30, 30, 10), 30)
        self.assertEqual(paeth(0, 3, 2), 0)
        pixels = [(10, 2, 5), (30, 3, 6), (80, 90, 100),
                  (30, 0, 7), (60, 70, 8), (120, 130, 140)]
        write_png(self.ref / "ties.png", 3, 2, pixels)
        write_png(self.run / "ties.png", 3, 2, pixels, filter_type=4)
        self.assert_exit(self.compare("--max-mean", 0), 0)

    def test_absolute_black_ceiling_identical_and_reference(self):
        # 99 % black: over the 98.5 % ceiling, but not flat (the flat check needs 99.5 %).
        dark = [(0, 0, 0)] * 198 + [(255, 255, 255)] * 2
        normal = [self.pixels[i % len(self.pixels)] for i in range(200)]
        write_png(self.ref / "identical.png", 20, 10, dark)
        write_png(self.run / "identical.png", 20, 10, dark)
        write_png(self.ref / "reference.png", 20, 10, dark)
        write_png(self.run / "reference.png", 20, 10, normal)
        report = self.root / "black.json"
        result = self.compare("--max-mean", 255, "--max-over64", 1, "--json", report)
        self.assert_exit(result, 1)
        entries = {e["path"]: e for e in json.loads(report.read_text(encoding="utf-8"))["files"]}
        for entry in entries.values():
            self.assertEqual(entry["status"], "FAIL")
            self.assertIn("black", entry["reason"].lower())
            self.assertNotIn("flat", entry["reason"].lower())

    def test_volatile_mostly_black_against_dark_reference(self):
        ref = [(0, 0, 0)] * 150 + [(120, 180, 220)] * 50
        run = [(0, 0, 0)] * 198 + [(255, 255, 255)] * 2
        write_png(self.ref / "volatile.png", 20, 10, ref)
        write_png(self.run / "volatile.png", 20, 10, run)
        baseline = self.root / "baseline.json"
        baseline.write_text(json.dumps({"files": {"volatile.png": {"stable": False}}}), encoding="utf-8")
        result = self.compare("--baseline", baseline)
        self.assert_exit(result, 1)
        self.assertIn("black", result.stdout.lower())
        self.assertNotIn("flat", result.stdout.lower())

    def test_emergency_picture_at_sizes_and_ring_phases(self):
        baseline = self.root / "baseline.json"
        baseline.write_text(json.dumps({"files": {"fallback.png": {"stable": False}}}), encoding="utf-8")
        for width, height in ((160, 90), (320, 180)):
            for phase in (-math.pi / 2, math.pi / 2):
                with self.subTest(size=(width, height), phase=phase):
                    fallback = fallback_pixels(width, height, phase)
                    normal = [self.pixels[i % len(self.pixels)] for i in range(width * height)]
                    for side in ("ref", "run"):
                        with self.subTest(side=side):
                            write_png(self.ref / "fallback.png", width, height, fallback if side == "ref" else normal)
                            write_png(self.run / "fallback.png", width, height, fallback if side == "run" else normal)
                            result = self.compare("--baseline", baseline)
                            self.assert_exit(result, 1)
                            self.assertIn("emergency test picture", result.stdout.lower())
        # An opposite-phase reject at another size must not hide the built-in failure.
        reject = self.root / "fallback-reject.png"
        write_png(reject, 160, 90, fallback_pixels(160, 90, -math.pi / 2))
        result = self.compare("--baseline", baseline, "--reject", reject)
        self.assert_exit(result, 1)
        self.assertIn("emergency test picture", result.stdout.lower())

    def test_reject_other_resolution_and_reference(self):
        width, height = 64, 36
        pixels = [(30 + (x // 8) * 15, 40 + (y // 6) * 20, 80)
                  for y in range(height) for x in range(width)]
        doubled = [pixels[(y // 2) * width + x // 2]
                   for y in range(height * 2) for x in range(width * 2)]
        reject = self.root / "reject.png"
        write_png(reject, width, height, pixels)
        write_png(self.ref / "frame.png", width * 2, height * 2, doubled)
        normal = [self.pixels[i % len(self.pixels)] for i in range(width * height * 4)]
        write_png(self.run / "frame.png", width * 2, height * 2, normal)
        result = self.compare("--reject", reject, "--max-mean", 255, "--max-over64", 1)
        self.assert_exit(result, 1)
        self.assertIn("reject", result.stdout.lower())
        write_png(self.ref / "frame.png", width * 2, height * 2, normal)
        write_png(self.run / "frame.png", width * 2, height * 2, doubled)
        result = self.compare("--reject", reject, "--max-mean", 255, "--max-over64", 1)
        self.assert_exit(result, 1)
        self.assertIn("reject", result.stdout.lower())

    def test_unreadable_reject_is_usage_error(self):
        self.png_pair()
        result = self.compare("--reject", self.root / "missing.png")
        self.assert_exit(result, 2)
        self.assertIn("reject", result.stderr.lower())

    def test_heatmap_colors_by_difference(self):
        ref = [(40, 40, 40)] * 4 + [(80, 80, 80), (100, 100, 100)]
        run = [(40, 40, 40), (48, 40, 40), (49, 40, 40), (104, 40, 40),
               (145, 80, 80), (100, 100, 100)]
        write_png(self.ref / "color.png", 3, 2, ref)
        write_png(self.run / "color.png", 3, 2, run)
        self.assert_exit(self.compare("--max-mean", 0, "--heatmaps"), 1)
        # Read via comparator's own PNG decoder to inspect generated RGB pixels.
        import importlib.util
        spec = importlib.util.spec_from_file_location("compare_captures", TOOL)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _, _, rows = module.read_image(self.run / "_diff" / "color.png.png")
        colors = [tuple(row[i:i + 3]) for row in rows for i in range(0, len(row), 3)]
        self.assertEqual(colors[0], (0, 0, 0))
        self.assertGreater(colors[1][2], colors[1][0])
        for color in colors[2:4]:
            self.assertGreater(color[0], color[2])
            self.assertGreater(color[1], color[2])
        self.assertGreater(colors[4][0], colors[4][1])
        self.assertGreater(colors[4][2], colors[4][1])

    def test_two_empty_folders_exit_two(self):
        # A capture run that wrote nothing must never count as a pass.
        self.assert_exit(self.compare(), 2)


if __name__ == "__main__":
    unittest.main()
