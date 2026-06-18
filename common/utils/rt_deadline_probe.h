#ifndef RT_DEADLINE_PROBE_H
#define RT_DEADLINE_PROBE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
  const char *name;
  int initialized;

  uint64_t total;
  uint64_t sum_us;
  uint64_t max_us;

  uint64_t over_100us;
  uint64_t over_200us;
  uint64_t over_500us;
  uint64_t over_1000us;

  uint64_t hist_0_50;
  uint64_t hist_50_100;
  uint64_t hist_100_200;
  uint64_t hist_200_500;
  uint64_t hist_500_1000;
  uint64_t hist_1000_2000;
  uint64_t hist_over_2000;

  uint64_t last_report_total;
} rt_deadline_probe_t;

static inline uint64_t rt_probe_now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t rt_probe_ns_to_us(uint64_t ns)
{
  return ns / 1000ULL;
}

static inline void rt_probe_init(rt_deadline_probe_t *p, const char *name)
{
  memset(p, 0, sizeof(*p));
  p->name = name;
  p->initialized = 1;
}

static inline void rt_probe_record(rt_deadline_probe_t *p, uint64_t duration_us)
{
  if (!p || !p->initialized)
    return;

  p->total++;
  p->sum_us += duration_us;

  if (duration_us > p->max_us)
    p->max_us = duration_us;

  if (duration_us > 100)
    p->over_100us++;
  if (duration_us > 200)
    p->over_200us++;
  if (duration_us > 500)
    p->over_500us++;
  if (duration_us > 1000)
    p->over_1000us++;

  if (duration_us < 50)
    p->hist_0_50++;
  else if (duration_us < 100)
    p->hist_50_100++;
  else if (duration_us < 200)
    p->hist_100_200++;
  else if (duration_us < 500)
    p->hist_200_500++;
  else if (duration_us < 1000)
    p->hist_500_1000++;
  else if (duration_us < 2000)
    p->hist_1000_2000++;
  else
    p->hist_over_2000++;
}

static inline void rt_probe_maybe_log_late(const rt_deadline_probe_t *p,
                                           int frame,
                                           int slot,
                                           uint64_t duration_us,
                                           uint64_t threshold_us)
{
  if (!p || !p->initialized)
    return;

  if (duration_us <= threshold_us)
    return;

  printf("RT_DEADLINE_LATE probe=%s frame=%d slot=%d duration_us=%lu threshold_us=%lu\n",
         p->name,
         frame,
         slot,
         duration_us,
         threshold_us);
  fflush(stdout);
}

static inline void rt_probe_maybe_report(rt_deadline_probe_t *p, uint64_t report_period_samples)
{
  if (!p || !p->initialized || report_period_samples == 0)
    return;

  if (p->total == 0)
    return;

  if ((p->total - p->last_report_total) < report_period_samples)
    return;

  p->last_report_total = p->total;

  const uint64_t avg_us = p->sum_us / p->total;

  printf("RT_DEADLINE_STATS probe=%s total=%lu avg_us=%lu max_us=%lu over_100us=%lu over_200us=%lu over_500us=%lu over_1000us=%lu hist_0_50=%lu hist_50_100=%lu hist_100_200=%lu hist_200_500=%lu hist_500_1000=%lu hist_1000_2000=%lu hist_over_2000=%lu\n",
         p->name,
         p->total,
         avg_us,
         p->max_us,
         p->over_100us,
         p->over_200us,
         p->over_500us,
         p->over_1000us,
         p->hist_0_50,
         p->hist_50_100,
         p->hist_100_200,
         p->hist_200_500,
         p->hist_500_1000,
         p->hist_1000_2000,
         p->hist_over_2000);
  fflush(stdout);
}

#endif /* RT_DEADLINE_PROBE_H */
