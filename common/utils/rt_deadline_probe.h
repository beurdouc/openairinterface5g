#ifndef RT_DEADLINE_PROBE_H
#define RT_DEADLINE_PROBE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RT_DEADLINE_NUM_THRESHOLDS 4
#define RT_DEADLINE_CAPTURE_PATH_MAX 256

typedef struct {
  int enabled;
  uint64_t report_period;
  uint64_t late_threshold_us;
  uint64_t threshold_us[RT_DEADLINE_NUM_THRESHOLDS];

  int capture_enable;
  int capture_snapshot_enable;
  int capture_final_dump_enable;
  uint64_t capture_samples;
  char capture_path[RT_DEADLINE_CAPTURE_PATH_MAX];
} rt_deadline_probe_config_t;

typedef struct {
  int valid;
  int frame;
  int slot;
  int dl_pdsch_count;
  int dl_prb_total;
  uint64_t dl_tbs_total;
  int dl_mcs_min;
  int dl_mcs_max;
  int dl_layers_max;
  int dl_rv_nonzero_count;
} rt_deadline_l1tx_context_t;

static inline rt_deadline_l1tx_context_t rt_deadline_l1tx_context_invalid(void)
{
  rt_deadline_l1tx_context_t ctx = {
      .valid = 0,
      .frame = -1,
      .slot = -1,
      .dl_pdsch_count = 0,
      .dl_prb_total = 0,
      .dl_tbs_total = 0,
      .dl_mcs_min = -1,
      .dl_mcs_max = -1,
      .dl_layers_max = -1,
      .dl_rv_nonzero_count = 0,
  };
  return ctx;
}

typedef struct {
  uint64_t capture_index;
  uint64_t probe_total;
  int frame;
  int slot;
  uint64_t duration_us;
  uint64_t late_threshold_us;
  int late;
  rt_deadline_l1tx_context_t ctx;
} rt_deadline_capture_sample_t;

static inline rt_deadline_probe_config_t rt_deadline_default_config(void)
{
  rt_deadline_probe_config_t cfg = {
      .enabled = 1,
      .report_period = 20000,
      .late_threshold_us = 500,
      .threshold_us = {100, 200, 500, 1000},
      .capture_enable = 0,
      .capture_snapshot_enable = 0,
      .capture_final_dump_enable = 1,
      .capture_samples = 20000,
      .capture_path = "/tmp/rt_deadline_samples.csv",
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

  rt_deadline_capture_sample_t *capture_buffer;
  uint64_t capture_count;
  uint64_t capture_capacity;
  uint64_t capture_last_dump_count;
  int capture_dumped;
  int capture_alloc_failed;
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

static inline void rt_probe_reset_capture(rt_deadline_probe_t *p)
{
  if (p == NULL)
    return;

  if (p->capture_buffer != NULL) {
    free(p->capture_buffer);
    p->capture_buffer = NULL;
  }

  p->capture_count = 0;
  p->capture_capacity = 0;
  p->capture_last_dump_count = 0;
  p->capture_dumped = 0;
  p->capture_alloc_failed = 0;
}

static inline void rt_probe_setup_capture(rt_deadline_probe_t *p)
{
  if (p == NULL)
    return;

  if (!p->cfg.capture_enable || p->cfg.capture_samples == 0)
    return;

  if (p->capture_buffer != NULL)
    return;

  if (p->cfg.capture_samples > (uint64_t)(SIZE_MAX / sizeof(*p->capture_buffer))) {
    p->capture_alloc_failed = 1;
    printf("RT_DEADLINE_CAPTURE_ERROR probe=%s reason=too_many_samples samples=%lu\n",
           p->name,
           p->cfg.capture_samples);
    fflush(stdout);
    return;
  }

  p->capture_buffer = calloc((size_t)p->cfg.capture_samples, sizeof(*p->capture_buffer));
  if (p->capture_buffer == NULL) {
    p->capture_alloc_failed = 1;
    printf("RT_DEADLINE_CAPTURE_ERROR probe=%s reason=alloc_failed samples=%lu\n",
           p->name,
           p->cfg.capture_samples);
    fflush(stdout);
    return;
  }

  p->capture_capacity = p->cfg.capture_samples;
  p->capture_count = 0;
  p->capture_dumped = 0;
  p->capture_alloc_failed = 0;

  printf("RT_DEADLINE_CAPTURE_CONFIG probe=%s enable=%d snapshot_enable=%d final_dump_enable=%d samples=%lu path=%s\n",
         p->name,
         p->cfg.capture_enable,
         p->cfg.capture_snapshot_enable,
         p->cfg.capture_final_dump_enable,
         p->cfg.capture_samples,
         p->cfg.capture_path);
  fflush(stdout);
}

static inline void rt_probe_set_config(rt_deadline_probe_t *p,
                                       const rt_deadline_probe_config_t *cfg)
{
  if (p == NULL || cfg == NULL)
    return;

  rt_probe_reset_capture(p);
  p->cfg = *cfg;
  rt_probe_setup_capture(p);
}

static inline void rt_probe_write_capture_csv(rt_deadline_probe_t *p, int final_dump)
{
  if (p == NULL || !p->initialized)
    return;

  if (!p->cfg.enabled || !p->cfg.capture_enable)
    return;

  if (p->capture_buffer == NULL || p->capture_count == 0)
    return;

  if (p->cfg.capture_path[0] == '\0')
    return;

  if (p->capture_dumped)
    return;

  char tmp_path[RT_DEADLINE_CAPTURE_PATH_MAX + 16];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", p->cfg.capture_path);

  FILE *f = fopen(tmp_path, "w");
  if (f == NULL) {
    printf("RT_DEADLINE_CAPTURE_ERROR probe=%s samples=%lu path=%s reason=fopen\n",
           p->name,
           p->capture_count,
           tmp_path);
    return;
  }

  fprintf(f, "capture_index,probe_total,frame,slot,duration_us,late_threshold_us,late,context_valid,dl_pdsch_count,dl_prb_total,dl_tbs_total,dl_mcs_min,dl_mcs_max,dl_layers_max,dl_rv_nonzero_count\n");

  for (uint64_t i = 0; i < p->capture_count; i++) {
    const rt_deadline_capture_sample_t *sample = &p->capture_buffer[i];
    fprintf(f,
            "%lu,%lu,%d,%d,%lu,%lu,%d,%d,%d,%d,%lu,%d,%d,%d,%d\n",
            sample->capture_index,
            sample->probe_total,
            sample->frame,
            sample->slot,
            sample->duration_us,
            sample->late_threshold_us,
            sample->late,
            sample->ctx.valid,
            sample->ctx.dl_pdsch_count,
            sample->ctx.dl_prb_total,
            sample->ctx.dl_tbs_total,
            sample->ctx.dl_mcs_min,
            sample->ctx.dl_mcs_max,
            sample->ctx.dl_layers_max,
            sample->ctx.dl_rv_nonzero_count);
  }

  if (fclose(f) != 0) {
    printf("RT_DEADLINE_CAPTURE_ERROR probe=%s samples=%lu path=%s reason=fclose\n",
           p->name,
           p->capture_count,
           tmp_path);
    remove(tmp_path);
    return;
  }

  if (rename(tmp_path, p->cfg.capture_path) != 0) {
    printf("RT_DEADLINE_CAPTURE_ERROR probe=%s samples=%lu path=%s reason=rename\n",
           p->name,
           p->capture_count,
           p->cfg.capture_path);
    remove(tmp_path);
    return;
  }

  p->capture_last_dump_count = p->capture_count;

  if (final_dump)
    p->capture_dumped = 1;

  printf("%s probe=%s samples=%lu capacity=%lu final=%d path=%s\n",
         final_dump ? "RT_DEADLINE_CAPTURE_DUMP" : "RT_DEADLINE_CAPTURE_SNAPSHOT",
         p->name,
         p->capture_count,
         p->capture_capacity,
         final_dump,
         p->cfg.capture_path);
}

static inline void rt_probe_dump_capture(rt_deadline_probe_t *p)
{
  if (p == NULL || !p->initialized)
    return;

  if (!p->cfg.enabled || !p->cfg.capture_enable)
    return;

  /*
   * Final dumps are performed during controlled shutdown, not periodically
   * from the realtime path. Keep this behavior explicit and configurable.
   */
  if (!p->cfg.capture_final_dump_enable)
    return;

  rt_probe_write_capture_csv(p, 1);
}

static inline void rt_probe_maybe_snapshot_capture(rt_deadline_probe_t *p)
{
  if (p == NULL || !p->initialized)
    return;

  if (!p->cfg.enabled || !p->cfg.capture_enable)
    return;

  /*
   * CSV snapshots perform synchronous file I/O from the realtime path.
   * Keep them disabled by default and require an explicit debug opt-in.
   */
  if (!p->cfg.capture_snapshot_enable)
    return;

  if (p->capture_buffer == NULL || p->capture_capacity == 0 || p->capture_dumped)
    return;

  if (p->capture_count == 0)
    return;

  if (p->capture_count >= p->capture_capacity) {
    rt_probe_write_capture_csv(p, 1);
    return;
  }

  if (p->cfg.report_period == 0)
    return;

  if (p->capture_count - p->capture_last_dump_count < p->cfg.report_period)
    return;

  rt_probe_write_capture_csv(p, 0);
}

static inline void rt_probe_capture_sample_with_l1tx_context(rt_deadline_probe_t *p,
                                                             int frame,
                                                             int slot,
                                                             uint64_t duration_us,
                                                             const rt_deadline_l1tx_context_t *ctx)
{
  if (p == NULL || !p->initialized)
    return;

  if (!p->cfg.enabled || !p->cfg.capture_enable)
    return;

  if (p->capture_buffer == NULL || p->capture_capacity == 0 || p->capture_dumped)
    return;

  if (p->capture_count >= p->capture_capacity) {
    rt_probe_maybe_snapshot_capture(p);
    return;
  }

  const uint64_t idx = p->capture_count;
  rt_deadline_capture_sample_t *sample = &p->capture_buffer[idx];

  sample->capture_index = idx;
  sample->probe_total = p->total;
  sample->frame = frame;
  sample->slot = slot;
  sample->duration_us = duration_us;
  sample->late_threshold_us = p->cfg.late_threshold_us;
  sample->late = p->cfg.late_threshold_us > 0 && duration_us > p->cfg.late_threshold_us;
  sample->ctx = ctx != NULL ? *ctx : rt_deadline_l1tx_context_invalid();

  p->capture_count++;

  rt_probe_maybe_snapshot_capture(p);
}

static inline void rt_probe_capture_sample(rt_deadline_probe_t *p,
                                           int frame,
                                           int slot,
                                           uint64_t duration_us)
{
  rt_deadline_l1tx_context_t ctx = rt_deadline_l1tx_context_invalid();
  rt_probe_capture_sample_with_l1tx_context(p, frame, slot, duration_us, &ctx);
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
