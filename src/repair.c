#include "repair.h"

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

void repair_init(RepairContext *ctx, unsigned psi_interval_ms, int verbose, int dry_run) {
    ctx->psi_interval_ms = psi_interval_ms;
    ctx->last_psi_inject_ms = 0;
    ctx->verbose = verbose;
    ctx->dry_run = dry_run;
}

uint64_t repair_now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
#endif
}

int repair_write_packet(RepairContext *ctx, TsOutput *out, const uint8_t packet[TS_PACKET_SIZE], Stats *stats) {
    if (!ctx->dry_run) {
        if (!ts_output_write_packet(out, packet)) {
            return 0;
        }
    }
    stats->packets_written++;
    return 1;
}

int repair_replace_tei_from_cache(RepairContext *ctx, const PsiContext *psi, const TsPacket *pkt,
                                  uint8_t packet[TS_PACKET_SIZE], Stats *stats) {
    const uint8_t *cached = NULL;
    const char *kind = NULL;

    if (!pkt->transport_error) {
        return 0;
    }

    if (pkt->pid == 0x0000 && psi->has_cached_pat) {
        cached = psi->cached_pat;
        kind = "PAT";
    } else if (psi_pid_is_pmt(psi, pkt->pid) && psi->has_cached_pmt) {
        cached = psi->cached_pmt;
        kind = "PMT";
    } else if (pkt->pid == 0x0011 && psi->has_cached_sdt) {
        cached = psi->cached_sdt;
        kind = "SDT";
    }

    if (!cached) {
        stats->unrepairable_transport_errors++;
        return 0;
    }

    for (size_t i = 0; i < TS_PACKET_SIZE; i++) {
        packet[i] = cached[i];
    }
    stats->psi_cache_repairs++;
    stats->repair_actions++;
    if (ctx->verbose) {
        fprintf(stderr, "[repair] action=replace_tei_from_cache pid=0x%04x table=%s reason=transport_error_indicator confidence=0.85\n",
                pkt->pid, kind);
    }
    return 1;
}

int repair_maybe_inject_psi(RepairContext *ctx, TsOutput *out, const PsiContext *psi, Stats *stats) {
    if (ctx->psi_interval_ms == 0 || (!psi->has_cached_pat && !psi->has_cached_pmt && !psi->has_cached_sdt)) {
        return 1;
    }

    uint64_t now = repair_now_ms();
    if (ctx->last_psi_inject_ms != 0 && now - ctx->last_psi_inject_ms < ctx->psi_interval_ms) {
        return 1;
    }

    int wrote = 0;
    if (psi->has_cached_pat) {
        if (!repair_write_packet(ctx, out, psi->cached_pat, stats)) return 0;
        wrote++;
    }
    if (psi->has_cached_pmt) {
        if (!repair_write_packet(ctx, out, psi->cached_pmt, stats)) return 0;
        wrote++;
    }
    if (psi->has_cached_sdt) {
        if (!repair_write_packet(ctx, out, psi->cached_sdt, stats)) return 0;
        wrote++;
    }

    if (wrote) {
        ctx->last_psi_inject_ms = now;
        stats->psi_injections += (uint64_t)wrote;
        stats->repair_actions += (uint64_t)wrote;
        if (ctx->verbose) {
            double confidence = psi->best_pat_confidence;
            if (psi->best_pmt_confidence > confidence) confidence = psi->best_pmt_confidence;
            if (psi->best_sdt_confidence > confidence) confidence = psi->best_sdt_confidence;
            fprintf(stderr, "[repair] action=inject_psi_si packets=%d reason=periodic_cache confidence=%.2f\n",
                    wrote, confidence);
        }
    }
    return 1;
}
