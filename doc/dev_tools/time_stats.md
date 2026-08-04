<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Processing times statistics with the homebrew time stats

## Purpose

The time stats tool is thought as an internal tool for recording statistics on processing stats of macroscopic computing blocks mostly for research purposes.
As internal tool, it is or can be freely adapted to measure timings according to specific needs of researchers or engineers.

**It is not** intended for debugging. For performance debugging tools, refer to the appropriate section of the [performance monitoring documentation](performance_monitoring.md).

## Features

* Timer based on Posix real time clocks.
* Accumulate successive measurements. Merge function enables to merge many timers measuring parallel processes.
* Provides average time, standard deviation and, optionally, time distribution.
* May be started and stopped on full DL or UL slots only for clean and relevant measurements.
* Already embedded in the stack to provide processing times of typical air interface processing blocks.

## Usage

A number of timers are already included in the nr-softmodem and PHY simulators to provide statistics on the main macroscopic blocks:
* In PHY simulators, option `-P` enables to display the statistics in the log.
* In the nr-softmodem, the time statistics are displayed in file `nrL1_stats.log` with a refresh every second when option `-q` is provided.
  This option may take an argument to display different statistics: 
  * Option `-q` or `-q 1` displays average, standard deviation and maximum of the measured times from softmodem start.  
  * Option `-q 2` displays average, standard deviation and distribution of the measured times recorded over one second.

The time stats tool is available through the header [time_meas.h](../../common/utils/time_meas.h).
It is implemented in this header and in the source file [time_meas.c](../../common/utils/time_meas.c).  
A timer is a typedef struct `time_stats_t` that can be started with `start_meas`, stopped with `stop_meas`, merged in another timer with `merge_meas` and reset with `reset_meas`.  
The start, stop and merge functions have `_on_dl` and `_on_ul` variants to measure only full DL or UL slots.  
Tracking the full distibution of processing times is optional and requires to enable the sorted list of the timer with `init_time_stats_sorted_list`.
Then the list shall be released with `free_time_stats_sorted_list` when finishing to use it.
