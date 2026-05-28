#ifndef CONTINUITY_H
#define CONTINUITY_H

#include <stdint.h>

#include "stats.h"
#include "ts_packet.h"

typedef struct {
    uint8_t seen;
    uint8_t last_cc;
    uint64_t packets;
    uint64_t missing;
    uint64_t duplicates;
    uint64_t out_of_order;
    uint64_t discontinuities;
} PidContinuity;

typedef struct {
    PidContinuity pid[TS_PID_COUNT];
} ContinuityContext;

void continuity_init(ContinuityContext *ctx);
void continuity_observe(ContinuityContext *ctx, const TsPacket *pkt, Stats *stats, int verbose);

#endif
