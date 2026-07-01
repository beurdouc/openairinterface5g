/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief Top-level threads for gNodeB
 */

#define _GNU_SOURCE
#undef MALLOC //there are two conflicting definitions, so we better make sure we don't use it at all

#include <fcntl.h> // for SEEK_SET
#include <pthread.h> // for pthread_join
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "common/utils/LOG/log.h"
#include "common/utils/system.h"
#include "PHY/NR_ESTIMATION/nr_ul_estimation.h"
#include "openair1/PHY/NR_TRANSPORT/nr_dlsch.h"
#include "openair1/PHY/NR_TRANSPORT/nr_ulsch.h"
#include "NR_PHY_INTERFACE/NR_IF_Module.h"
#include "PHY/INIT/nr_phy_init.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/TOOLS/tools_defs.h"
#include "PHY/defs_RU.h"
#include "PHY/defs_gNB.h"
#include "PHY/defs_nr_common.h"
#include "PHY/impl_defs_nr.h"
#include "PHY/nr_phy_common/inc/nr_phy_meas.h"
#include "SCHED_NR/phy_frame_config_nr.h"
#include "SCHED_NR/sched_nr.h"
#include "assertions.h"
#include "common/ran_context.h"
#include "common/utils/LOG/log.h"
#include "executables/softmodem-common.h"
#include "nfapi/oai_integration/vendor_ext.h"
#include "nfapi_nr_interface_scf.h"
#include "notified_fifo.h"
#include "thread-pool.h"
#include "time_meas.h"
#include "common/config/config_userapi.h"
#include "utils.h"

#define TICK_TO_US(ts) (ts.trials==0?0:ts.diff/ts.trials)
#define L1STATSSTRLEN 16384
static void rx_func(processingData_L1_t *param);

static rt_deadline_probe_config_t g_l1tx_rt_deadline_cfg;
static int g_l1tx_rt_deadline_cfg_loaded = 0;

static void load_l1tx_rt_deadline_config_once(void)
{
  if (g_l1tx_rt_deadline_cfg_loaded)
    return;

  g_l1tx_rt_deadline_cfg = rt_deadline_default_config();

  /*
   * L1_TX_JOB_DL measures the full gNB DL TX job around tx_func(info).
   * It has a larger time scale than the RU sub-probes.
   * Keep 500 us as an observation threshold, but use 1000 us as the
   * default late logging threshold to avoid excessive RT_DEADLINE_LATE logs.
   */
  g_l1tx_rt_deadline_cfg.enabled = 1;
  g_l1tx_rt_deadline_cfg.report_period = 20000;
  g_l1tx_rt_deadline_cfg.late_threshold_us = 1000;
  g_l1tx_rt_deadline_cfg.threshold_us[0] = 200;
  g_l1tx_rt_deadline_cfg.threshold_us[1] = 500;
  g_l1tx_rt_deadline_cfg.threshold_us[2] = 1000;
  g_l1tx_rt_deadline_cfg.threshold_us[3] = 2000;
  g_l1tx_rt_deadline_cfg.capture_enable = 0;
  g_l1tx_rt_deadline_cfg.capture_snapshot_enable = 0;
  g_l1tx_rt_deadline_cfg.capture_async_flush_enable = 0;
  g_l1tx_rt_deadline_cfg.capture_final_dump_enable = 1;
  g_l1tx_rt_deadline_cfg.capture_samples = 20000;
  snprintf(g_l1tx_rt_deadline_cfg.capture_path,
           sizeof(g_l1tx_rt_deadline_cfg.capture_path),
           "/tmp/rt_deadline_l1tx_samples.csv");

  int enabled = g_l1tx_rt_deadline_cfg.enabled;
  int report_period = (int)g_l1tx_rt_deadline_cfg.report_period;
  int late_threshold_us = (int)g_l1tx_rt_deadline_cfg.late_threshold_us;
  int threshold0_us = (int)g_l1tx_rt_deadline_cfg.threshold_us[0];
  int threshold1_us = (int)g_l1tx_rt_deadline_cfg.threshold_us[1];
  int threshold2_us = (int)g_l1tx_rt_deadline_cfg.threshold_us[2];
  int threshold3_us = (int)g_l1tx_rt_deadline_cfg.threshold_us[3];
  int capture_enable = g_l1tx_rt_deadline_cfg.capture_enable;
  int capture_snapshot_enable = g_l1tx_rt_deadline_cfg.capture_snapshot_enable;
  int capture_async_flush_enable = g_l1tx_rt_deadline_cfg.capture_async_flush_enable;
  int capture_final_dump_enable = g_l1tx_rt_deadline_cfg.capture_final_dump_enable;
  int capture_samples = (int)g_l1tx_rt_deadline_cfg.capture_samples;

  paramdef_t RTDeadlineL1TXParams[] = {
    {"enable", NULL, 0, .iptr = &enabled, .defintval = enabled, TYPE_INT, 0, NULL},
    {"report_period", NULL, 0, .iptr = &report_period, .defintval = report_period, TYPE_INT, 0, NULL},
    {"late_threshold_us", NULL, 0, .iptr = &late_threshold_us, .defintval = late_threshold_us, TYPE_INT, 0, NULL},
    {"threshold0_us", NULL, 0, .iptr = &threshold0_us, .defintval = threshold0_us, TYPE_INT, 0, NULL},
    {"threshold1_us", NULL, 0, .iptr = &threshold1_us, .defintval = threshold1_us, TYPE_INT, 0, NULL},
    {"threshold2_us", NULL, 0, .iptr = &threshold2_us, .defintval = threshold2_us, TYPE_INT, 0, NULL},
    {"threshold3_us", NULL, 0, .iptr = &threshold3_us, .defintval = threshold3_us, TYPE_INT, 0, NULL},
    {"capture_enable", NULL, 0, .iptr = &capture_enable, .defintval = capture_enable, TYPE_INT, 0, NULL},
    {"capture_snapshot_enable", NULL, 0, .iptr = &capture_snapshot_enable, .defintval = capture_snapshot_enable, TYPE_INT, 0, NULL},
    {"capture_async_flush_enable", NULL, 0, .iptr = &capture_async_flush_enable, .defintval = capture_async_flush_enable, TYPE_INT, 0, NULL},
    {"capture_final_dump_enable", NULL, 0, .iptr = &capture_final_dump_enable, .defintval = capture_final_dump_enable, TYPE_INT, 0, NULL},
    {"capture_samples", NULL, 0, .iptr = &capture_samples, .defintval = capture_samples, TYPE_INT, 0, NULL},
  };

  config_get(config_get_if(), RTDeadlineL1TXParams, sizeofArray(RTDeadlineL1TXParams), "rt_deadline_l1tx");

  g_l1tx_rt_deadline_cfg.enabled = enabled;
  g_l1tx_rt_deadline_cfg.report_period = report_period > 0 ? (uint64_t)report_period : g_l1tx_rt_deadline_cfg.report_period;
  g_l1tx_rt_deadline_cfg.late_threshold_us = late_threshold_us > 0 ? (uint64_t)late_threshold_us : g_l1tx_rt_deadline_cfg.late_threshold_us;
  g_l1tx_rt_deadline_cfg.threshold_us[0] = threshold0_us > 0 ? (uint64_t)threshold0_us : g_l1tx_rt_deadline_cfg.threshold_us[0];
  g_l1tx_rt_deadline_cfg.threshold_us[1] = threshold1_us > 0 ? (uint64_t)threshold1_us : g_l1tx_rt_deadline_cfg.threshold_us[1];
  g_l1tx_rt_deadline_cfg.threshold_us[2] = threshold2_us > 0 ? (uint64_t)threshold2_us : g_l1tx_rt_deadline_cfg.threshold_us[2];
  g_l1tx_rt_deadline_cfg.threshold_us[3] = threshold3_us > 0 ? (uint64_t)threshold3_us : g_l1tx_rt_deadline_cfg.threshold_us[3];
  g_l1tx_rt_deadline_cfg.capture_enable = capture_enable;
  g_l1tx_rt_deadline_cfg.capture_snapshot_enable = capture_snapshot_enable;
  g_l1tx_rt_deadline_cfg.capture_async_flush_enable = capture_async_flush_enable;
  g_l1tx_rt_deadline_cfg.capture_final_dump_enable = capture_final_dump_enable;
  g_l1tx_rt_deadline_cfg.capture_samples = capture_samples > 0 ? (uint64_t)capture_samples : g_l1tx_rt_deadline_cfg.capture_samples;

  printf("RT_DEADLINE_CONFIG_L1TX enabled=%d report_period=%lu late_threshold_us=%lu "
         "threshold0_us=%lu threshold1_us=%lu threshold2_us=%lu threshold3_us=%lu "
         "capture_enable=%d capture_snapshot_enable=%d capture_async_flush_enable=%d capture_final_dump_enable=%d capture_samples=%lu capture_path=%s\n",
         g_l1tx_rt_deadline_cfg.enabled,
         g_l1tx_rt_deadline_cfg.report_period,
         g_l1tx_rt_deadline_cfg.late_threshold_us,
         g_l1tx_rt_deadline_cfg.threshold_us[0],
         g_l1tx_rt_deadline_cfg.threshold_us[1],
         g_l1tx_rt_deadline_cfg.threshold_us[2],
         g_l1tx_rt_deadline_cfg.threshold_us[3],
         g_l1tx_rt_deadline_cfg.capture_enable,
         g_l1tx_rt_deadline_cfg.capture_snapshot_enable,
         g_l1tx_rt_deadline_cfg.capture_async_flush_enable,
         g_l1tx_rt_deadline_cfg.capture_final_dump_enable,
         g_l1tx_rt_deadline_cfg.capture_samples,
         g_l1tx_rt_deadline_cfg.capture_path);
  fflush(stdout);

  g_l1tx_rt_deadline_cfg_loaded = 1;
}

static void configure_gnb_l1tx_rt_probe(PHY_VARS_gNB *gNB)
{
  if (!gNB)
    return;

  load_l1tx_rt_deadline_config_once();
  rt_probe_set_config(&gNB->rt_l1_tx_job_probe, &g_l1tx_rt_deadline_cfg);
}



static void tx_func(processingData_L1tx_t *info)
{
  int frame_tx = info->frame;
  int slot_tx = info->slot;
  int frame_rx = info->frame_rx;
  int slot_rx = info->slot_rx;
  LOG_D(NR_PHY, "%d.%d running tx_func\n", frame_tx, slot_tx);
  PHY_VARS_gNB *gNB = info->gNB;
  NR_IF_Module_t *ifi = gNB->if_inst;
  nfapi_nr_config_request_scf_t *cfg = &gNB->gNB_config;

  T(T_GNB_PHY_DL_TICK, T_INT(gNB->Mod_id), T_INT(frame_tx), T_INT(slot_tx));

  if (slot_rx == 0) {
    reset_active_stats(gNB, frame_rx);
    reset_active_ulsch(gNB, frame_rx);
  }

  clear_slot_beamid(gNB, slot_tx);

  nfapi_nr_slot_indication_scf_t ind = {.header = {.phy_id = 0}, .sfn = frame_tx, .slot = slot_tx};
  start_meas(&gNB->slot_indication_stats);
  // this variable is very big (multiple MB), so we put it into static storage
  // to not overflow the stack while still having it in local (function) scope
  // also, tx_func() is only executed by one thread, serially
  static NR_Sched_Rsp_t sched_response;
  ifi->NR_slot_indication(&ind, &sched_response);
  stop_meas(&gNB->slot_indication_stats);

  info->gNB = gNB;

  // At this point, MAC scheduler just ran, including scheduling
  // PRACH/PUCCH/PUSCH, so trigger RX chain processing
  nr_save_ul_tti_req(gNB, &sched_response.UL_tti_req);
  LOG_D(NR_PHY, "Trigger RX for %d.%d\n", frame_rx, slot_rx);
  notifiedFIFO_elt_t *res = newNotifiedFIFO_elt(sizeof(processingData_L1_t), 0, &gNB->resp_L1, NULL);
  processingData_L1_t *syncMsg = NotifiedFifoData(res);
  syncMsg->gNB = gNB;
  syncMsg->frame_rx = frame_rx;
  syncMsg->slot_rx = slot_rx;
  syncMsg->timestamp_tx = info->timestamp_tx;
  res->key = slot_rx;
  pushNotifiedFIFO(&gNB->resp_L1, res);

  int tx_slot_type = nr_slot_select(cfg, frame_tx, slot_tx);
  // TODO check for analog_bf_vendor_ext set to 1 is a workaround while no beam API for beam selection is implemented
  if (tx_slot_type == NR_DOWNLINK_SLOT || tx_slot_type == NR_MIXED_SLOT || get_softmodem_params()->continuous_tx
      || IS_SOFTMODEM_RFSIM || cfg->analog_beamforming_ve.analog_bf_vendor_ext.value) {
    START_MEAS_FULL_SLOT(&info->gNB->phy_proc_tx, tx_slot_type, NR_DOWNLINK_SLOT);
    START_MEAS_FULL_SLOT(&info->gNB->gnb_tx_procedures_stats, tx_slot_type, NR_DOWNLINK_SLOT);
    phy_procedures_gNB_TX(info->gNB, &sched_response.DL_req, &sched_response.TX_req, &sched_response.UL_dci_req, frame_tx,slot_tx);
    STOP_MEAS_FULL_SLOT(&info->gNB->gnb_tx_procedures_stats, tx_slot_type, NR_DOWNLINK_SLOT);

    PHY_VARS_gNB *gNB = info->gNB;
    processingData_RU_t syncMsgRU;
    syncMsgRU.frame_tx = frame_tx;
    syncMsgRU.slot_tx = slot_tx;
    syncMsgRU.ru = gNB->RU_list[0];
    syncMsgRU.timestamp_tx = info->timestamp_tx;
    LOG_D(PHY, "gNB: %d.%d : calling RU TX function\n", syncMsgRU.frame_tx, syncMsgRU.slot_tx);

    START_MEAS_FULL_SLOT(&info->gNB->ru_tx_func_stats, tx_slot_type, NR_DOWNLINK_SLOT);

    ru_tx_func((void *)&syncMsgRU);

    STOP_MEAS_FULL_SLOT(&info->gNB->ru_tx_func_stats, tx_slot_type, NR_DOWNLINK_SLOT);
    STOP_MEAS_FULL_SLOT(&info->gNB->phy_proc_tx, tx_slot_type, NR_DOWNLINK_SLOT);
  }
}

void *L1_rx_thread(void *arg) 
{
  PHY_VARS_gNB *gNB = (PHY_VARS_gNB*)arg;

  while (oai_exit == 0) {
     notifiedFIFO_elt_t *res = pullNotifiedFIFO(&gNB->resp_L1);
     if (res == NULL)
       break;
     processingData_L1_t *info = (processingData_L1_t *)NotifiedFifoData(res);
     int slot_type = nr_slot_select(&gNB->gNB_config, info->frame_rx, info->slot_rx);
     START_MEAS_FULL_SLOT(&gNB->l1_rx_proc, slot_type, NR_UPLINK_SLOT);
     rx_func(info);
     STOP_MEAS_FULL_SLOT(&gNB->l1_rx_proc, slot_type, NR_UPLINK_SLOT);
     delNotifiedFIFO_elt(res);
  }
  return NULL;
}
// Added for URLLC, requires MAC scheduling to be split from UL indication
void *L1_tx_thread(void *arg) {
  PHY_VARS_gNB *gNB = (PHY_VARS_gNB*)arg;

  while (oai_exit == 0) {
     notifiedFIFO_elt_t *res = pullNotifiedFIFO(&gNB->L1_tx_out);
     if (res == NULL) // stopping condition, happens only when queue is freed
       break;
     processingData_L1tx_t *info = (processingData_L1tx_t *)NotifiedFifoData(res);
     int slot_type = nr_slot_select(&gNB->gNB_config, info->frame, info->slot);
     uint64_t rt_l1_tx_start_ns = 0;

     if (slot_type == NR_DOWNLINK_SLOT) {
       if (!gNB->rt_l1_tx_job_probe.initialized) {
         rt_probe_init(&gNB->rt_l1_tx_job_probe, "L1_TX_JOB_DL");
         configure_gnb_l1tx_rt_probe(gNB);
       }

       rt_l1_tx_start_ns = rt_probe_now_ns();
     }
     START_MEAS_FULL_SLOT(&gNB->l1_tx_proc, slot_type, NR_DOWNLINK_SLOT);

     tx_func(info);

     STOP_MEAS_FULL_SLOT(&gNB->l1_tx_proc, slot_type, NR_DOWNLINK_SLOT);
     if (slot_type == NR_DOWNLINK_SLOT) {

       const uint64_t rt_l1_tx_duration_us = rt_probe_ns_to_us(rt_probe_now_ns() - rt_l1_tx_start_ns);
       rt_probe_record(&gNB->rt_l1_tx_job_probe, rt_l1_tx_duration_us);
       rt_deadline_l1tx_context_t rt_l1tx_ctx = rt_deadline_l1tx_context_invalid();
       if (gNB->rt_l1tx_slot_context.valid &&
           gNB->rt_l1tx_slot_context.frame == info->frame &&
           gNB->rt_l1tx_slot_context.slot == info->slot)
         rt_l1tx_ctx = gNB->rt_l1tx_slot_context;

       rt_probe_capture_sample_with_l1tx_context(&gNB->rt_l1_tx_job_probe,
                                                 info->frame,
                                                 info->slot,
                                                 rt_l1_tx_duration_us,
                                                 &rt_l1tx_ctx);
       rt_probe_maybe_log_late(&gNB->rt_l1_tx_job_probe, info->frame, info->slot, rt_l1_tx_duration_us, g_l1tx_rt_deadline_cfg.late_threshold_us);
       rt_probe_maybe_report(&gNB->rt_l1_tx_job_probe, g_l1tx_rt_deadline_cfg.report_period);
     }

     delNotifiedFIFO_elt(res);
  }

  if (gNB->rt_l1_tx_job_probe.initialized)
    rt_probe_dump_capture(&gNB->rt_l1_tx_job_probe);

  return NULL;
}

static void rx_func(processingData_L1_t *info)
{
  PHY_VARS_gNB *gNB = info->gNB;
  int frame_rx = info->frame_rx;
  int slot_rx = info->slot_rx;
  nfapi_nr_config_request_scf_t *cfg = &gNB->gNB_config;

  T(T_GNB_PHY_UL_TICK, T_INT(gNB->Mod_id), T_INT(frame_rx), T_INT(slot_rx));

  // RX processing
  int rx_slot_type = nr_slot_select(cfg, frame_rx, slot_rx);
  if (rx_slot_type == NR_UPLINK_SLOT || rx_slot_type == NR_MIXED_SLOT) {
    LOG_D(NR_PHY, "%d.%d Starting RX processing\n", frame_rx, slot_rx);

    // UE-specific RX processing for subframe n
    NR_UL_IND_t UL_INFO = {.frame = frame_rx, .slot = slot_rx, .module_id = gNB->Mod_id, .phy_id = gNB->CC_id};
    // Do PRACH RU processing
    UL_INFO.rach_ind.pdu_list = UL_INFO.prach_pdu_indication_list;
    UL_INFO.rach_ind.number_of_pdus = 0;
    // even if processing is late, we might collect all PRACH
    // the last PRACH's frame/slot is when all UE's appear to have accessed
    prach_item_t p;
    while (spsc_q_get(&gNB->prach_l1rx_queue, &p, sizeof(p)))
      L1_nr_prach_procedures(gNB, &p, &UL_INFO.rach_ind);

    //WA: comment rotation in tx/rx
    if (gNB->phase_comp) {
      //apply the rx signal rotation here
      int soffset = (slot_rx % RU_RX_SLOT_DEPTH) * gNB->frame_parms.symbols_per_slot * gNB->frame_parms.ofdm_symbol_size;
      const NR_DL_FRAME_PARMS *fp = &gNB->frame_parms;
      for (int aa = 0; aa < fp->nb_antennas_rx; aa++) {
        const uint max_symb = fp->Ncp == NR_EXTENDED ? 12 : 14;
        for (int sym = 0; sym < max_symb; sym++)
          apply_nr_rotation_symbol_RX(fp->symbols_per_slot,
                                      fp->slots_per_subframe,
                                      fp->timeshift_symbol_rotation,
                                      fp->first_carrier_offset,
                                      gNB->common_vars.rxdataF[aa] + soffset + sym * fp->ofdm_symbol_size,
                                      fp->symbol_rotation[1],
                                      fp->N_RB_UL,
                                      slot_rx,
                                      sym);
      }
    }
    phy_procedures_gNB_uespec_RX(gNB, frame_rx, slot_rx, &UL_INFO);

    // Call the scheduler
    START_MEAS_FULL_SLOT(&gNB->ul_indication_stats, rx_slot_type, NR_UPLINK_SLOT);
    gNB->if_inst->NR_UL_indication(&UL_INFO);
    STOP_MEAS_FULL_SLOT(&gNB->ul_indication_stats, rx_slot_type, NR_UPLINK_SLOT);

    notifiedFIFO_elt_t *res = newNotifiedFIFO_elt(sizeof(processingData_L1_t), 0, &gNB->L1_rx_out, NULL);
    processingData_L1_t *syncMsg = NotifiedFifoData(res);
    syncMsg->gNB = gNB;
    syncMsg->frame_rx = frame_rx;
    syncMsg->slot_rx = slot_rx;
    res->key = slot_rx;
    LOG_D(NR_PHY, "Signaling completion for %d.%d (mod_slot %d) on L1_rx_out\n", frame_rx, slot_rx, slot_rx % RU_RX_SLOT_DEPTH);
    pushNotifiedFIFO(&gNB->L1_rx_out, res);
  }

}

static void nrL1_stats_init_sorted_list(PHY_VARS_gNB *gNB, RU_t *ru, unsigned int size)
{
  init_sorted_list_meas(&gNB->l1_tx_proc, size);
  init_sorted_list_meas(&gNB->l1_rx_proc, size);
  init_sorted_list_meas(&gNB->phy_proc_tx, size);
  init_sorted_list_meas(&gNB->gnb_tx_procedures_stats, size);
  init_sorted_list_meas(&gNB->ru_tx_func_stats, size);
  init_sorted_list_meas(&gNB->dlsch_encoding_stats, size);
  init_sorted_list_meas(&gNB->dlsch_ldpc_encode_stats, size);
  init_sorted_list_meas(&gNB->dlsch_scrambling_stats, size);
  init_sorted_list_meas(&gNB->dlsch_modulation_stats, size);
  init_sorted_list_meas(&gNB->dlsch_pdsch_generation_stats, size);
  init_sorted_list_meas(&gNB->phy_proc_rx, size);
  init_sorted_list_meas(&gNB->ulsch_decoding_stats, size);
  init_sorted_list_meas(&gNB->ts_ldpc_decode, size);
  init_sorted_list_meas(&gNB->ul_indication_stats, size);
  init_sorted_list_meas(&gNB->slot_indication_stats, size);
  init_sorted_list_meas(&gNB->rx_pusch_stats, size);
  init_sorted_list_meas(&gNB->rx_prach, size);
  if (ru->feprx) {
    init_sorted_list_meas(&ru->ofdm_demod_stats, size);
  }
  if (ru->feptx_prec) {
    init_sorted_list_meas(&ru->precoding_stats, size);
  }
  if (ru->feptx_ofdm) {
    init_sorted_list_meas(&ru->txdataF_copy_stats, size);
    init_sorted_list_meas(&ru->ofdm_mod_stats, size);
    init_sorted_list_meas(&ru->ofdm_total_stats, size);
  }
  init_sorted_list_meas(&ru->tx_fhaul, size);
}

static void nrL1_stats_free_sorted_list(PHY_VARS_gNB *gNB, RU_t *ru)
{
  free_sorted_list_meas(&gNB->l1_tx_proc);
  free_sorted_list_meas(&gNB->l1_rx_proc);
  free_sorted_list_meas(&gNB->phy_proc_tx);
  free_sorted_list_meas(&gNB->gnb_tx_procedures_stats);
  free_sorted_list_meas(&gNB->ru_tx_func_stats);
  free_sorted_list_meas(&gNB->dlsch_encoding_stats);
  free_sorted_list_meas(&gNB->dlsch_ldpc_encode_stats);
  free_sorted_list_meas(&gNB->dlsch_scrambling_stats);
  free_sorted_list_meas(&gNB->dlsch_modulation_stats);
  free_sorted_list_meas(&gNB->dlsch_pdsch_generation_stats);
  free_sorted_list_meas(&gNB->phy_proc_rx);
  free_sorted_list_meas(&gNB->ulsch_decoding_stats);
  free_sorted_list_meas(&gNB->ts_ldpc_decode);
  free_sorted_list_meas(&gNB->ul_indication_stats);
  free_sorted_list_meas(&gNB->slot_indication_stats);
  free_sorted_list_meas(&gNB->rx_pusch_stats);
  free_sorted_list_meas(&gNB->rx_prach);
  if (ru->feprx) {
    free_sorted_list_meas(&ru->ofdm_demod_stats);
  }
  if (ru->feptx_prec) {
    free_sorted_list_meas(&ru->precoding_stats);
  }
  if (ru->feptx_ofdm) {
    free_sorted_list_meas(&ru->txdataF_copy_stats);
    free_sorted_list_meas(&ru->ofdm_mod_stats);
    free_sorted_list_meas(&ru->ofdm_total_stats);
    free_sorted_list_meas(&ru->txdataF_copy_stats);
  }
  free_sorted_list_meas(&ru->tx_fhaul);
}

static void nrL1_stats_reset(PHY_VARS_gNB *gNB, RU_t *ru)
{
  reset_meas(&gNB->l1_tx_proc);
  reset_meas(&gNB->l1_rx_proc);
  reset_meas(&gNB->phy_proc_tx);
  reset_meas(&gNB->gnb_tx_procedures_stats);
  reset_meas(&gNB->ru_tx_func_stats);
  reset_meas(&gNB->dlsch_encoding_stats);
  reset_meas(&gNB->dlsch_ldpc_encode_stats);
  reset_meas(&gNB->dlsch_scrambling_stats);
  reset_meas(&gNB->dlsch_modulation_stats);
  reset_meas(&gNB->dlsch_resource_mapping_stats);
  reset_meas(&gNB->dlsch_pdsch_generation_stats);
  reset_meas(&gNB->phy_proc_rx);
  reset_meas(&gNB->ulsch_decoding_stats);
  reset_meas(&gNB->ts_ldpc_decode);
  reset_meas(&gNB->ul_indication_stats);
  reset_meas(&gNB->slot_indication_stats);
  reset_meas(&gNB->rx_pusch_stats);
  reset_meas(&gNB->rx_prach);
  if (ru->feprx) {
    reset_meas(&ru->ofdm_demod_stats);
  }
  if (ru->feptx_prec) {
    reset_meas(&ru->precoding_stats);
  }
  if (ru->feptx_ofdm) {
    reset_meas(&ru->txdataF_copy_stats);
    reset_meas(&ru->ofdm_mod_stats);
    reset_meas(&ru->ofdm_total_stats);
    reset_meas(&ru->txdataF_copy_stats);
  }
  reset_meas(&ru->tx_fhaul);
}

static size_t dump_L1_meas_stats(PHY_VARS_gNB *gNB, RU_t *ru, char *output, size_t outputlen) {
  const char *begin = output;
  const char *end = output + outputlen;
  output += print_meas_log_header(NULL, NULL, output, end - output, cpu_meas_enabled);
  output += print_meas_log(&gNB->l1_tx_proc, "L1 Tx job", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->l1_rx_proc, "L1 Rx job", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->phy_proc_tx, "L1 Tx processing", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->gnb_tx_procedures_stats,
                           "L1 gNB TX procedures",
                           NULL,
                           NULL,
                           output,
                           end - output);

  output += print_meas_log(&gNB->ru_tx_func_stats,
                           "L1 RU TX function",
                           NULL,
                           NULL,
                           output,
                           end - output);
  output += print_meas_log(&gNB->dlsch_encoding_stats, "DLSCH encoding", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->dlsch_ldpc_encode_stats, "LDPC encoding", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->dlsch_scrambling_stats, "DLSCH scrambling", NULL, NULL, output, end-output);
  output += print_meas_log(&gNB->dlsch_modulation_stats, "DLSCH modulation", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->dlsch_pdsch_generation_stats, "PDSCH generation", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->phy_proc_rx, "L1 Rx processing", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->ulsch_decoding_stats, "ULSCH decoding", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->ts_ldpc_decode, "UL segments decoding", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->ul_indication_stats, "UL Indication", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->slot_indication_stats, "Slot Indication", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->rx_pusch_stats, "PUSCH inner-receiver", NULL, NULL, output, end - output);
  output += print_meas_log(&gNB->rx_prach, "PRACH RX", NULL, NULL, output, end - output);
  if (ru->feprx) {
    output += print_meas_log(&ru->ofdm_demod_stats, "feprx", NULL, NULL, output, end - output);
  }

  bool full_slot = ru->half_slot_parallelization == 0;
  if (ru->feptx_prec) {
    output += print_meas_log(&ru->precoding_stats,
                             full_slot ? "feptx_prec (per port)" : "feptx_prec (per port, half_slot)",
                             NULL,
                             NULL,
                             output,
                             end - output);
  }

  if (ru->feptx_ofdm) {
    output += print_meas_log(&ru->txdataF_copy_stats,"txdataF_copy",NULL,NULL, output, end - output);
    output += print_meas_log(&ru->ofdm_mod_stats,
                             full_slot ? "feptx_ofdm (per port)" : "feptx_ofdm (per port, half_slot)",
                             NULL,
                             NULL,
                             output,
                             end - output);
    output += print_meas_log(&ru->ofdm_total_stats,"feptx_total",NULL,NULL, output, end - output);
    output += print_meas_log(&ru->txdataF_copy_stats, "txdataF_copy", NULL, NULL, output, end - output);
  }

  output += print_meas_log(&ru->tx_fhaul,"tx_fhaul",NULL,NULL, output, end - output);

  if (cpu_meas_enabled == TIME_STATS_ADVANCED_MODE) {
    nrL1_stats_reset(gNB, ru);
  }
  return output - begin;
}

#define SORTED_LIST_SIZE 2048
void *nrL1_stats_thread(void *param) {
  PHY_VARS_gNB     *gNB      = (PHY_VARS_gNB *)param;
  RU_t *ru = RC.ru[0];
  char output[L1STATSSTRLEN];
  memset(output,0,L1STATSSTRLEN);
  wait_sync("L1_stats_thread");
  FILE *fd=fopen("nrL1_stats.log","w");
  if (!fd) {
    LOG_W(NR_PHY, "Cannot open nrL1_stats.log: %d, %s\n", errno, strerror(errno));
    return NULL;
  }

  if (cpu_meas_enabled == TIME_STATS_ADVANCED_MODE) {
    nrL1_stats_init_sorted_list(gNB, ru, SORTED_LIST_SIZE);
  }

  nrL1_stats_reset(gNB, ru);

  while (!oai_exit) {
    sleep(1);
    if (ftruncate(fileno(fd), 0) != 0 || fseek(fd, 0, SEEK_SET) != 0) {
      LOG_E(NR_MAC, "error while writing nrL1_stats.log: %d, %s\n", errno, strerror(errno));
      break;
    }
    dump_nr_I0_stats(fd,gNB);
    dump_pdsch_stats(fd,gNB);
    dump_pusch_stats(fd,gNB);
    dump_L1_meas_stats(gNB, ru, output, L1STATSSTRLEN);
    fprintf(fd,"%s\n",output);
    fflush(fd);
    /*
     * Flush L1TX deadline samples from the low-priority stats thread.
     * The realtime L1TX path only writes to the bounded memory buffer.
     */
    rt_probe_flush_capture_csv(&gNB->rt_l1_tx_job_probe, 0);

  }

  if (cpu_meas_enabled == TIME_STATS_ADVANCED_MODE) {
    nrL1_stats_free_sorted_list(gNB, ru);
  }

  fclose(fd);
  return(NULL);
}

void init_gNB_Tpool(int inst)
{
  AssertFatal(NFAPI_MODE == NFAPI_MODE_PNF || NFAPI_MODE == NFAPI_MONOLITHIC,
              "illegal NFAPI_MODE %d (%s): it cannot have an L1\n",
              NFAPI_MODE,
              nfapi_get_strmode());

  PHY_VARS_gNB *gNB;
  gNB = RC.gNB[inst];
  gNB_L1_proc_t *proc = &gNB->proc;
  // ULSCH decoding threadpool
  initTpool(get_softmodem_params()->threadPoolConfig, &gNB->threadPool, cpumeas(CPUMEAS_GETSTATE));

  // L1 RX result FIFO
  initNotifiedFIFO(&gNB->resp_L1);
  // L1 TX result FIFO
  initNotifiedFIFO(&gNB->L1_tx_out);
  initNotifiedFIFO(&gNB->L1_rx_out);

  // create the RX thread responsible for RX processing start event (resp_L1 msg queue), then launch rx_func()
  threadCreate(&gNB->L1_rx_thread, L1_rx_thread, (void *)gNB, "L1_rx_thread", gNB->L1_rx_thread_core, OAI_PRIORITY_RT_MAX);
  // create the TX thread responsible for TX processing start event (L1_tx_out msg queue), then launch tx_func()
  threadCreate(&gNB->L1_tx_thread, L1_tx_thread, (void *)gNB, "L1_tx_thread", gNB->L1_tx_thread_core, OAI_PRIORITY_RT_MAX);

  if (!IS_SOFTMODEM_NOSTATS)
    threadCreate(&proc->L1_stats_thread, nrL1_stats_thread, (void *)gNB, "L1_stats", -1, OAI_PRIORITY_RT_LOW);
}

void term_gNB_Tpool(int inst) {
  PHY_VARS_gNB *gNB = RC.gNB[inst];
  abortNotifiedFIFO(&gNB->resp_L1);
  pthread_join(gNB->L1_rx_thread, NULL);
  abortNotifiedFIFO(&gNB->L1_tx_out);
  pthread_join(gNB->L1_tx_thread, NULL);

  abortTpool(&gNB->threadPool);
  abortNotifiedFIFO(&gNB->L1_rx_out);

  gNB_L1_proc_t *proc = &gNB->proc;
  pthread_join(proc->L1_stats_thread, NULL);
}

/// eNB kept in function name for nffapi calls, TO FIX
void init_eNB_afterRU(void)
{
  for (int inst = 0; inst < RC.nb_nr_L1_inst; inst++) {
    PHY_VARS_gNB *gNB = RC.gNB[inst];
    phy_init_nr_gNB(gNB);

    // map antennas and PRACH signals to gNB RX
    if (0) AssertFatal(gNB->num_RU>0,"Number of RU attached to gNB %d is zero\n",gNB->Mod_id);

    LOG_D(NR_PHY, "Mapping RX ports from %d RUs to gNB %d\n", gNB->num_RU, gNB->Mod_id);
    int aa = 0;
    for (int ru_id = 0; ru_id < gNB->num_RU; ru_id++) {
      AssertFatal(gNB->RU_list[ru_id]->common.rxdataF != NULL, "RU %d : common.rxdataF is NULL\n", gNB->RU_list[ru_id]->idx);
      for (int i = 0; i < gNB->RU_list[ru_id]->nb_rx; aa++, i++) {
        LOG_I(PHY, "Attaching RU %d antenna %d to gNB antenna %d\n", gNB->RU_list[ru_id]->idx, i, aa);
        gNB->common_vars.rxdataF[aa] = (c16_t *)gNB->RU_list[ru_id]->common.rxdataF[i];
      }
    }
    /* TODO: review this code, there is something wrong.
     * In monolithic mode, we come here with nb_antennas_rx == 0
     * (not tested in other modes).
     */
    //init_precoding_weights(RC.gNB[inst]);
    init_gNB_Tpool(inst);
  }
}

/**
 * @brief Initialize gNB struct in RAN context
 */
void init_gNB()
{
  LOG_I(NR_PHY, "Initializing gNB RAN context: RC.nb_nr_L1_inst = %d \n", RC.nb_nr_L1_inst);
  if (RC.gNB == NULL) {
    RC.gNB = (PHY_VARS_gNB **)calloc_or_fail(RC.nb_nr_L1_inst, sizeof(PHY_VARS_gNB *));
    LOG_D(NR_PHY, "gNB L1 structure RC.gNB allocated @ %p\n", RC.gNB);
  }

  for (int inst = 0; inst < RC.nb_nr_L1_inst; inst++) {
    // Allocate L1 instance
    if (RC.gNB[inst] == NULL) {
      RC.gNB[inst] = (PHY_VARS_gNB *)calloc_or_fail(1, sizeof(PHY_VARS_gNB));
      LOG_D(NR_PHY, "[nr-gnb.c] gNB structure RC.gNB[%d] allocated @ %p\n", inst, RC.gNB[inst]);
    }
    PHY_VARS_gNB *gNB = RC.gNB[inst];
    LOG_D(NR_PHY, "Initializing gNB %d\n", inst);

    // Init module ID
    gNB->Mod_id = inst;

    // Register MAC interface module
    AssertFatal((gNB->if_inst = NR_IF_Module_init(inst)) != NULL, "Cannot register interface");

    LOG_I(NR_PHY, "Registered with MAC interface module (%p)\n", gNB->if_inst);
    gNB->if_inst->NR_PHY_config_req = nr_phy_config_request;

    gNB->prach_energy_counter = 0;
    gNB->chest_time = get_softmodem_params()->chest_time;
    gNB->chest_freq = get_softmodem_params()->chest_freq;
  }
}

void stop_gNB(int nb_inst) {
  for (int inst=0; inst<nb_inst; inst++) {
    LOG_I(PHY,"Killing gNB %d processing threads\n",inst);
    term_gNB_Tpool(inst);
  }
}
