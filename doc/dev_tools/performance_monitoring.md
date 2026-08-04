<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Tools for performance monitoring

Several tools can be used for performance analysis of OpenAirInterface.
Different tools have different purposes.

## Debugging purpose

* A profiler such as [perf](https://perfwiki.github.io/main/) enables to dissect the contribution of processes and their different parts to processing times and computing resource usage.
* A tracers such as [Tracy](tracy.md) enables an inclusive and in depth monitoring of the performance of a specified part of software.

## Benchmarking purpose

* The [time stats tool](time_stats.md) is a homebrew embedded processing time recorder for macroscopic computing blocks.
  It is thought for high-level benchmarking and research purposes as it provides a high-level view of processing times of typical air interface processing blocks.
