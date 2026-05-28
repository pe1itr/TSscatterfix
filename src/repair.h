#ifndef REPAIR_H
#define REPAIR_H

#include <stdint.h>

#include "io.h"
#include "psi.h"
#include "stats.h"

typedef struct {
    unsigned psi_interval_ms;
    uint64_t last_psi_inject_ms;
    int verbose;
    int dry_run;
} RepairContext;

void repair_init(RepairContext *ctx, unsigned psi_interval_ms, int verbose, int dry_run);
uint64_t repair_now_ms(void);
int repair_maybe_inject_psi(RepairContext *ctx, TsOutput *out, const PsiContext *psi, Stats *stats);
int repair_write_packet(RepairContext *ctx, TsOutput *out, const uint8_t packet[TS_PACKET_SIZE], Stats *stats);
int repair_replace_tei_from_cache(RepairContext *ctx, const PsiContext *psi, const TsPacket *pkt,
                                  uint8_t packet[TS_PACKET_SIZE], Stats *stats);

#endif
