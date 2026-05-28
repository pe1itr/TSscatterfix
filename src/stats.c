#include "stats.h"

#include "ml_model.h"

#include <inttypes.h>
#include <string.h>

void stats_init(Stats *stats) {
    memset(stats, 0, sizeof(*stats));
}

void stats_print(FILE *err, const Stats *stats, const PsiContext *psi, uint64_t interval_index) {
    fprintf(err,
            "[stats #%" PRIu64 "] read=%" PRIu64 " written=%" PRIu64 " dropped=%" PRIu64
            " drop_bytes=%" PRIu64 " resync=%" PRIu64 " cc_errors=%" PRIu64
            " missing=%" PRIu64 " dup=%" PRIu64 " ooo=%" PRIu64 " psi_inj=%" PRIu64 "\n",
            interval_index, stats->packets_read, stats->packets_written, stats->packets_dropped,
            stats->bytes_dropped, stats->resync_count, stats->continuity_errors,
            stats->missing_packets, stats->duplicates, stats->out_of_order, stats->psi_injections);

    fprintf(err,
            "[repairable] invalid_ts=%" PRIu64 " tei=%" PRIu64
            " psi_cache_repairs=%" PRIu64 " unrepairable_tei=%" PRIu64 "\n",
            stats->invalid_packets, stats->transport_error_packets,
            stats->psi_cache_repairs, stats->unrepairable_transport_errors);

    if (stats->clean_ts_null_packets || stats->clean_ts_filtered_pids ||
        stats->clean_ts_psi_cache_repairs || stats->clean_ts_bad_psi_nulls ||
        stats->clean_ts_media_cc_rewrites || stats->clean_ts_pts_rewrites ||
        stats->clean_ts_pcr_rewrites) {
        fprintf(err,
                "[clean-ts] null_packets=%" PRIu64 " filtered_pid_packets=%" PRIu64
                " psi_cache_repairs=%" PRIu64 " bad_psi_nulls=%" PRIu64
                " media_cc_rewrites=%" PRIu64 " pts_rewrites=%" PRIu64
                " pcr_rewrites=%" PRIu64 "\n",
                stats->clean_ts_null_packets, stats->clean_ts_filtered_pids,
                stats->clean_ts_psi_cache_repairs, stats->clean_ts_bad_psi_nulls,
                stats->clean_ts_media_cc_rewrites, stats->clean_ts_pts_rewrites,
                stats->clean_ts_pcr_rewrites);
    }

    fprintf(err,
            "[repeat] packet_candidates=%" PRIu64 " payload_candidates=%" PRIu64 "\n",
            stats->repeated_packet_candidates, stats->repeated_payload_candidates);

    fprintf(err, "[psi] pat=%s pmt=%s sdt=%s pmt_pid=0x%04x pcr_pid=0x%04x best_pat=%.3f best_pmt=%.3f best_sdt=%.3f\n",
            psi->has_pat ? "yes" : "no", psi->has_pmt ? "yes" : "no",
            psi->has_sdt ? "yes" : "no", psi->primary_pmt_pid, psi->pcr_pid,
            psi->best_pat_confidence, psi->best_pmt_confidence, psi->best_sdt_confidence);

    if (psi->has_sdt) {
        fprintf(err, "[psi] service_id=0x%04x provider=\"%s\" service=\"%s\"\n",
                psi->service_id, psi->provider_name, psi->service_name);
    }

    if (psi->video_count > 0) {
        fprintf(err, "[psi] video_pids:");
        for (size_t i = 0; i < psi->video_count; i++) {
            fprintf(err, " 0x%04x", psi->video_pids[i]);
        }
        fprintf(err, "\n");
    }
    if (psi->audio_count > 0) {
        fprintf(err, "[psi] audio_pids:");
        for (size_t i = 0; i < psi->audio_count; i++) {
            fprintf(err, " 0x%04x", psi->audio_pids[i]);
        }
        fprintf(err, "\n");
    }
}
