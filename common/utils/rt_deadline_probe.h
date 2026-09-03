#ifndef RT_DEADLINE_PROBE_H
#define RT_DEADLINE_PROBE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RT_DEADLINE_NUM_THRESHOLDS 4

typedef struct {
  int enabled;
  uint64_t report_period;
  uint64_t late_threshold_us;
  uint64_t threshold_us[RT_DEADLINE_NUM_THRESHOLDS];
} rt_deadline_probe_config_t;

static inline rt_deadline_probe_config_t rt_deadline_default_config(void)
{
  rt_deadline_probe_config_t cfg = {
      .enabled = 1,
      .report_period = 20000,
      .late_threshold_us = 500,
      .threshold_us = {100, 200, 500, 1000},
  };
  return cfg;
}

typedef struct {
  const char *name;
  int initialized;

  rt_deadline_probe_config_t cfg;

  uint64_t total;
  uint64_t sum_us;
  uint64_t max_us;
  uint64_t late_count;

  uint64_t over_100us;
  uint64_t over_200us;
  uint64_t over_500us;
  uint64_t over_1000us;

  uint64_t over_threshold[RT_DEADLINE_NUM_THRESHOLDS];

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

static inline uint64_t rt_probe_ratio_ppm(uint64_t count, uint64_t total)
{
  if (total == 0)
    return 0;

  return (count * 1000000ULL + total / 2) / total;
}

static inline void rt_probe_init(rt_deadline_probe_t *p, const char *name)
{
  memset(p, 0, sizeof(*p));
  p->name = name;
  p->initialized = 1;
  p->cfg = rt_deadline_default_config();
}

static inline void rt_probe_set_config(rt_deadline_probe_t *p,
                                       const rt_deadline_probe_config_t *cfg)
{
  if (p == NULL || cfg == NULL)
    return;

  p->cfg = *cfg;
}

static inline void rt_probe_record(rt_deadline_probe_t *p, uint64_t duration_us)
{
  if (!p || !p->initialized)
    return;

  if (!p->cfg.enabled)
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

  if (p->cfg.late_threshold_us > 0 && duration_us > p->cfg.late_threshold_us)
    p->late_count++;

  for (int i = 0; i < RT_DEADLINE_NUM_THRESHOLDS; i++) {
    if (p->cfg.threshold_us[i] > 0 && duration_us > p->cfg.threshold_us[i])
      p->over_threshold[i]++;
  }

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

  if (!p->cfg.enabled)
    return;

  if (p->cfg.late_threshold_us > 0)
    threshold_us = p->cfg.late_threshold_us;

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
  if (!p || !p->initialized)
    return;

  if (!p->cfg.enabled)
    return;

  if (p->cfg.report_period > 0)
    report_period_samples = p->cfg.report_period;

  if (report_period_samples == 0)
    return;

  if (p->total == 0)
    return;

  if ((p->total - p->last_report_total) < report_period_samples)
    return;

  p->last_report_total = p->total;

  const uint64_t avg_us = p->sum_us / p->total;
  const uint64_t late_ratio_ppm = rt_probe_ratio_ppm(p->late_count, p->total);
  const uint64_t over_threshold0_ratio_ppm = rt_probe_ratio_ppm(p->over_threshold[0], p->total);
  const uint64_t over_threshold1_ratio_ppm = rt_probe_ratio_ppm(p->over_threshold[1], p->total);
  const uint64_t over_threshold2_ratio_ppm = rt_probe_ratio_ppm(p->over_threshold[2], p->total);
  const uint64_t over_threshold3_ratio_ppm = rt_probe_ratio_ppm(p->over_threshold[3], p->total);

  printf("RT_DEADLINE_STATS probe=%s total=%lu avg_us=%lu max_us=%lu "
         "enabled=%d report_period=%lu late_threshold_us=%lu "
         "late_count=%lu late_ratio_ppm=%lu "
         "over_100us=%lu over_200us=%lu over_500us=%lu over_1000us=%lu "
         "threshold0_us=%lu over_threshold0=%lu over_threshold0_ratio_ppm=%lu "
         "threshold1_us=%lu over_threshold1=%lu over_threshold1_ratio_ppm=%lu "
         "threshold2_us=%lu over_threshold2=%lu over_threshold2_ratio_ppm=%lu "
         "threshold3_us=%lu over_threshold3=%lu over_threshold3_ratio_ppm=%lu "
         "hist_0_50=%lu hist_50_100=%lu hist_100_200=%lu "
         "hist_200_500=%lu hist_500_1000=%lu hist_1000_2000=%lu hist_over_2000=%lu\n",
         p->name,
         p->total,
         avg_us,
         p->max_us,
         p->cfg.enabled,
         p->cfg.report_period,
         p->cfg.late_threshold_us,
         p->late_count,
         late_ratio_ppm,
         p->over_100us,
         p->over_200us,
         p->over_500us,
         p->over_1000us,
         p->cfg.threshold_us[0],
         p->over_threshold[0],
         over_threshold0_ratio_ppm,
         p->cfg.threshold_us[1],
         p->over_threshold[1],
         over_threshold1_ratio_ppm,
         p->cfg.threshold_us[2],
         p->over_threshold[2],
         over_threshold2_ratio_ppm,
         p->cfg.threshold_us[3],
         p->over_threshold[3],
         over_threshold3_ratio_ppm,
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
