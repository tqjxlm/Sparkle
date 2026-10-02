"""Tests for the per-encoder integration of Apple GPU bandwidth counters in dev/apple_gpu_traffic.py."""

import os
import sys
import tempfile
import unittest
from xml.sax.saxutils import escape

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "dev"))
import apple_gpu_traffic  # noqa: E402


def table(columns, rows):
    """An xctrace export of one table; a cell repeating an earlier element of its column refers to it, as xctrace
    does."""
    schema = "".join(f"<col><mnemonic>{column}</mnemonic></col>" for column in columns)
    seen, body, next_id = {}, [], 1
    for row in rows:
        cells = []
        for column, value in zip(columns, row):
            if (column, value) in seen:
                cells.append(f'<cell ref="{seen[column, value]}"/>')
            else:
                seen[column, value] = next_id
                cells.append(f'<cell id="{next_id}" fmt="{escape(str(value))}">{escape(str(value))}</cell>')
                next_id += 1
        body.append(f"<row>{''.join(cells)}</row>")
    return f'<?xml version="1.0"?><trace-query-result><node><schema>{schema}</schema>{"".join(body)}</node>' \
        "</trace-query-result>"


def measure(encoders, intervals, counters):
    with tempfile.TemporaryDirectory() as directory:
        paths = {}
        for schema, text in (("metal-application-encoders-list", encoders), ("metal-gpu-intervals", intervals),
                             ("metal-gpu-counter-intervals", counters)):
            paths[schema] = os.path.join(directory, f"{schema}.xml")
            with open(paths[schema], "w", encoding="utf-8") as file:
                file.write(text)
        return apple_gpu_traffic.measure(paths)


class MeasureTest(unittest.TestCase):
    def test_integrates_bandwidth_over_each_encoders_gpu_time(self):
        encoders = table(["process", "encoder-id", "encoder-label"],
                         [("sparkle (1)", "e1", "Shadow"), ("sparkle (1)", "e2", "Main"),
                          ("WindowServer (2)", "w1", "Compose")])
        # e1's vertex and fragment work overlap and count once; w1 overlaps 10 ns of e2
        intervals = table(["start", "duration", "encoder-id", "frame-number"],
                          [(0, 100, "e1", "Frame 1"), (50, 100, "e1", "Frame 1"), (200, 100, "e2", "Frame 1"),
                           (250, 10, "w1", "Frame 9")])
        counters = table(["start", "duration", "name", "value"],
                         [(0, 100, "GPU Write Bandwidth", 2.0), (100, 100, "GPU Write Bandwidth", 4.0),
                          (200, 100, "GPU Write Bandwidth", 1.0), (0, 300, "GPU Read Bandwidth", 1.0),
                          (0, 300, "GPU Bandwidth", 9.0)])
        per_label, frames = measure(encoders, intervals, counters)

        self.assertEqual(frames, 1)
        self.assertEqual(set(per_label), {"Shadow", "Main"})
        # a sample's bytes go to the work running in it by time; w1 takes its share of the samples it ran in
        self.assertAlmostEqual(per_label["Shadow"]["write"], 2.0 * 100 + 4.0 * 100)
        self.assertAlmostEqual(per_label["Main"]["write"], 1.0 * 100 * 100 / 110)
        self.assertAlmostEqual(per_label["Shadow"]["read"], 1.0 * 300 * 150 / 260)
        self.assertAlmostEqual(per_label["Main"]["read"], 1.0 * 300 * 100 / 260)
        self.assertAlmostEqual(per_label["Shadow"]["gpu_ns"], 150.0)
        self.assertAlmostEqual(per_label["Shadow"]["shared_ns"], 0.0)
        self.assertAlmostEqual(per_label["Main"]["shared_ns"], 10.0)

    def test_scales_bytes_to_the_gpu_time_the_counters_did_not_sample(self):
        encoders = table(["process", "encoder-id", "encoder-label"], [("sparkle (1)", "e1", "Main")])
        intervals = table(["start", "duration", "encoder-id", "frame-number"], [(0, 100, "e1", "Frame 1")])
        counters = table(["start", "duration", "name", "value"],
                         [(0, 50, "GPU Write Bandwidth", 2.0), (0, 100, "GPU Read Bandwidth", 1.0)])
        per_label, _ = measure(encoders, intervals, counters)

        self.assertAlmostEqual(per_label["Main"]["write"], 2.0 * 50 * 100 / 50)
        self.assertAlmostEqual(per_label["Main"]["read"], 100.0)


if __name__ == "__main__":
    unittest.main()
