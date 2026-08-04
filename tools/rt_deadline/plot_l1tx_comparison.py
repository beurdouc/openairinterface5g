#!/usr/bin/env python3
# SPDX-License-Identifier: OAI-OpenAirInterface

import argparse
import sys
from pathlib import Path
from typing import List, Tuple

from analyze_l1tx_capture import load_capture


def parse_run(value: str) -> Tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(
            "--run must use the format NAME=/path/to/capture.csv"
        )

    name, path = value.split("=", 1)
    name = name.strip()

    if not name:
        raise argparse.ArgumentTypeError("run name must not be empty")
    if not path:
        raise argparse.ArgumentTypeError("run CSV path must not be empty")

    return name, Path(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot comparative L1_TX_JOB_DL latency distributions."
    )
    parser.add_argument(
        "--run",
        type=parse_run,
        action="append",
        required=True,
        help="Run in the form NAME=/path/to/capture.csv. Repeat for each run.",
    )
    parser.add_argument(
        "--deadline-us",
        type=int,
        action="append",
        default=None,
        help="Deadline marker in microseconds. May be repeated.",
    )
    parser.add_argument(
        "--x-max-us",
        type=int,
        default=None,
        help="Optional upper bound of the x-axis in microseconds.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Output PNG path.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        if len(args.run) < 2:
            raise ValueError("at least two --run arguments are required")

        names = [name for name, _ in args.run]
        if len(names) != len(set(names)):
            raise ValueError("run names must be unique")

        if args.x_max_us is not None and args.x_max_us <= 0:
            raise ValueError("--x-max-us must be > 0")

        deadlines: List[int] = sorted(set(args.deadline_us or []))
        if any(deadline <= 0 for deadline in deadlines):
            raise ValueError("--deadline-us values must be > 0")

        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        figure, axis = plt.subplots()

        for name, csv_path in args.run:
            rows = load_capture(csv_path)
            durations = sorted(row["duration_us"] for row in rows)
            cumulative = [
                (index + 1) / len(durations)
                for index in range(len(durations))
            ]

            axis.step(
                durations,
                cumulative,
                where="post",
                label=f"{name} (n={len(durations)})",
            )

        for deadline_us in deadlines:
            if args.x_max_us is None or deadline_us <= args.x_max_us:
                axis.axvline(
                    deadline_us,
                    linestyle="--",
                    linewidth=1,
                    label=f"deadline {deadline_us} us",
                )

        axis.set_xlabel("L1 TX processing duration (us)")
        axis.set_ylabel("Empirical cumulative probability")
        axis.set_ylim(0.0, 1.0)

        if args.x_max_us is not None:
            axis.set_xlim(right=args.x_max_us)

        axis.legend()
        figure.tight_layout()

        args.output.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(args.output, dpi=160)
        plt.close(figure)

        print(f"ecdf_comparison_plot={args.output}")
        return 0

    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
