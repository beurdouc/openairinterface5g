#!/usr/bin/env python3
# SPDX-License-Identifier: OAI-OpenAirInterface
#
# Summarize bounded L1_TX_JOB_DL deadline capture CSV files.
#
# Input CSV format:
#   capture_index,probe_total,frame,slot,duration_us,late_threshold_us,late

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import Dict, List, Sequence, Tuple


REQUIRED_COLUMNS = {
    "capture_index",
    "probe_total",
    "frame",
    "slot",
    "duration_us",
    "late_threshold_us",
    "late",
}


def ratio_ppm(count: int, total: int) -> int:
    if total == 0:
        return 0
    return int((count * 1_000_000 + total // 2) // total)


def percentile_nearest_rank(sorted_values: Sequence[int], percentile: float) -> int:
    if not sorted_values:
        raise ValueError("empty value list")

    if percentile <= 0:
        return sorted_values[0]
    if percentile >= 100:
        return sorted_values[-1]

    rank = math.ceil((percentile / 100.0) * len(sorted_values))
    index = max(0, min(rank - 1, len(sorted_values) - 1))
    return sorted_values[index]


def load_capture(path: Path) -> List[Dict[str, int]]:
    rows: List[Dict[str, int]] = []

    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError("missing CSV header")

        missing = REQUIRED_COLUMNS.difference(reader.fieldnames)
        if missing:
            raise ValueError(f"missing required columns: {', '.join(sorted(missing))}")

        for line_no, row in enumerate(reader, start=2):
            try:
                parsed = {
                    "capture_index": int(row["capture_index"]),
                    "probe_total": int(row["probe_total"]),
                    "frame": int(row["frame"]),
                    "slot": int(row["slot"]),
                    "duration_us": int(row["duration_us"]),
                    "late_threshold_us": int(row["late_threshold_us"]),
                    "late": int(row["late"]),
                }
            except (TypeError, ValueError) as exc:
                raise ValueError(f"invalid integer value at CSV line {line_no}: {exc}") from exc

            rows.append(parsed)

    if not rows:
        raise ValueError("CSV contains no samples")

    return rows


def validate_order(rows: Sequence[Dict[str, int]]) -> List[str]:
    warnings: List[str] = []

    for expected, row in enumerate(rows):
        if row["capture_index"] != expected:
            warnings.append(
                f"capture_index discontinuity at sample {expected}: "
                f"got {row['capture_index']}"
            )
            break

    previous_probe_total = rows[0]["probe_total"]
    for row in rows[1:]:
        if row["probe_total"] <= previous_probe_total:
            warnings.append(
                "probe_total is not strictly increasing "
                f"near capture_index={row['capture_index']}"
            )
            break
        previous_probe_total = row["probe_total"]

    return warnings


def burst_stats(rows: Sequence[Dict[str, int]], threshold_us: int) -> Tuple[int, int, int]:
    burst_count = 0
    longest_burst = 0
    current_burst = 0

    for row in rows:
        if row["duration_us"] > threshold_us:
            current_burst += 1
            if current_burst == 1:
                burst_count += 1
            longest_burst = max(longest_burst, current_burst)
        else:
            current_burst = 0

    over_count = sum(1 for row in rows if row["duration_us"] > threshold_us)
    return over_count, burst_count, longest_burst


def summarize(rows: Sequence[Dict[str, int]], thresholds: Sequence[int]) -> str:
    durations = [row["duration_us"] for row in rows]
    sorted_durations = sorted(durations)
    total = len(rows)

    late_count = sum(1 for row in rows if row["late"] != 0)
    late_thresholds = sorted(set(row["late_threshold_us"] for row in rows))

    lines: List[str] = []
    lines.append("RT_DEADLINE_CAPTURE_SUMMARY")
    lines.append(f"samples={total}")
    lines.append(f"first_capture_index={rows[0]['capture_index']}")
    lines.append(f"last_capture_index={rows[-1]['capture_index']}")
    lines.append(f"first_probe_total={rows[0]['probe_total']}")
    lines.append(f"last_probe_total={rows[-1]['probe_total']}")
    lines.append(f"first_frame={rows[0]['frame']}")
    lines.append(f"first_slot={rows[0]['slot']}")
    lines.append(f"last_frame={rows[-1]['frame']}")
    lines.append(f"last_slot={rows[-1]['slot']}")
    lines.append(f"min_us={min(durations)}")
    lines.append(f"avg_us={sum(durations) / total:.3f}")
    lines.append(f"max_us={max(durations)}")
    lines.append(f"p50_us={percentile_nearest_rank(sorted_durations, 50)}")
    lines.append(f"p90_us={percentile_nearest_rank(sorted_durations, 90)}")
    lines.append(f"p99_us={percentile_nearest_rank(sorted_durations, 99)}")
    lines.append(f"p999_us={percentile_nearest_rank(sorted_durations, 99.9)}")
    lines.append(f"p9999_us={percentile_nearest_rank(sorted_durations, 99.99)}")
    lines.append(f"late_threshold_us_values={','.join(str(x) for x in late_thresholds)}")
    lines.append(f"late_count={late_count}")
    lines.append(f"late_ratio_ppm={ratio_ppm(late_count, total)}")

    for threshold_us in thresholds:
        over_count, burst_count, longest_burst = burst_stats(rows, threshold_us)
        lines.append(
            "threshold "
            f"threshold_us={threshold_us} "
            f"count={over_count} "
            f"ratio_ppm={ratio_ppm(over_count, total)} "
            f"burst_count={burst_count} "
            f"longest_burst={longest_burst}"
        )

    warnings = validate_order(rows)
    for warning in warnings:
        lines.append(f"warning={warning}")

    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize L1_TX_JOB_DL deadline capture CSV samples."
    )
    parser.add_argument(
        "csv_path",
        type=Path,
        help="Path to rt_deadline_l1tx_samples.csv",
    )
    parser.add_argument(
        "--threshold",
        dest="thresholds",
        type=int,
        action="append",
        default=None,
        help="Threshold in microseconds. Can be provided multiple times.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    thresholds = args.thresholds if args.thresholds is not None else [200, 500, 1000, 2000]
    thresholds = sorted(set(thresholds))

    try:
        rows = load_capture(args.csv_path)
        print(summarize(rows, thresholds))
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
