#include "continuity.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void continuity_init(ContinuityContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

void continuity_observe(ContinuityContext *ctx, const TsPacket *pkt, Stats *stats, int verbose) {
    if (pkt->pid >= TS_PID_COUNT || pkt->pid == TS_NULL_PID) {
        return;
    }

    PidContinuity *p = &ctx->pid[pkt->pid];
    p->packets++;

    if (pkt->discontinuity_indicator) {
        p->discontinuities++;
        if (verbose) {
            fprintf(stderr, "[cc] pid=0x%04x discontinuity_indicator set; accepting new continuity base cc=%u\n",
                    pkt->pid, pkt->continuity_counter);
        }
        p->seen = 1;
        p->last_cc = pkt->continuity_counter;
        return;
    }

    if (!pkt->has_payload) {
        return;
    }

    if (!p->seen) {
        p->seen = 1;
        p->last_cc = pkt->continuity_counter;
        return;
    }

    uint8_t expected = (uint8_t)((p->last_cc + 1) & 0x0f);
    if (pkt->continuity_counter == expected) {
        p->last_cc = pkt->continuity_counter;
        return;
    }

    if (pkt->continuity_counter == p->last_cc) {
        p->duplicates++;
        stats->duplicates++;
        stats->continuity_errors++;
        if (verbose) {
            fprintf(stderr, "[cc] pid=0x%04x duplicate cc=%u action=pass confidence=0.75\n",
                    pkt->pid, pkt->continuity_counter);
        }
        return;
    }

    uint8_t delta = (uint8_t)((pkt->continuity_counter - expected) & 0x0f);
    if (delta < 8) {
        uint64_t missing = (uint64_t)delta + 1u;
        p->missing += missing;
        stats->missing_packets += missing;
        stats->continuity_errors++;
        if (verbose) {
            fprintf(stderr, "[cc] pid=0x%04x missing=%" PRIu64 " expected=%u got=%u action=log confidence=0.80\n",
                    pkt->pid, missing, expected, pkt->continuity_counter);
        }
    } else {
        p->out_of_order++;
        stats->out_of_order++;
        stats->continuity_errors++;
        if (verbose) {
            fprintf(stderr, "[cc] pid=0x%04x out_of_order expected=%u got=%u action=pass confidence=0.60\n",
                    pkt->pid, expected, pkt->continuity_counter);
        }
    }
    p->last_cc = pkt->continuity_counter;
}
