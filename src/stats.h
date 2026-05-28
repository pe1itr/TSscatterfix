#ifndef STATS_H
#define STATS_H

#include <stdint.h>
#include <stdio.h>

#include "psi.h"

typedef struct {
    uint64_t packets_read;
    uint64_t packets_written;
    uint64_t packets_dropped;
    uint64_t bytes_dropped;
    uint64_t resync_count;
    uint64_t invalid_packets;
    uint64_t transport_error_packets;
    uint64_t unrepairable_transport_errors;
    uint64_t psi_cache_repairs;
    uint64_t repeated_packet_candidates;
    uint64_t repeated_payload_candidates;
    uint64_t continuity_errors;
    uint64_t duplicates;
    uint64_t out_of_order;
    uint64_t missing_packets;
    uint64_t psi_injections;
    uint64_t repair_actions;
    uint64_t clean_ts_null_packets;
    uint64_t clean_ts_filtered_pids;
    uint64_t clean_ts_psi_cache_repairs;
    uint64_t clean_ts_bad_psi_nulls;
    uint64_t clean_ts_media_cc_rewrites;
    uint64_t clean_ts_pts_rewrites;
    uint64_t clean_ts_pcr_rewrites;
} Stats;

void stats_init(Stats *stats);
void stats_print(FILE *err, const Stats *stats, const PsiContext *psi, uint64_t interval_index);

#endif
