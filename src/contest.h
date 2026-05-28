#ifndef CONTEST_H
#define CONTEST_H

#include <stdint.h>
#include <stdio.h>

#include "psi.h"
#include "stats.h"
#include "ts_packet.h"

#define CONTEST_FRAGMENT_CAPACITY 4096
#define CONTEST_FRAGMENT_PAYLOAD_MAX 184

typedef struct {
    uint64_t packet_index;
    uint16_t pid;
    uint8_t continuity_counter;
    uint8_t payload_unit_start;
    uint8_t transport_error;
    uint8_t scrambling;
    uint8_t discontinuity_indicator;
    uint8_t payload_size;
    uint32_t payload_hash;
    double confidence;
    uint8_t payload[CONTEST_FRAGMENT_PAYLOAD_MAX];
} ContestFragment;

typedef struct {
    ContestFragment fragments[CONTEST_FRAGMENT_CAPACITY];
    uint64_t fragments_seen;
    uint64_t fragments_stored;
    uint64_t bytes_stored;
    uint64_t tei_fragments;
    uint64_t pusi_fragments;
    uint64_t scrambled_fragments;
    uint64_t discontinuity_fragments;
    uint64_t video_fragments;
    uint64_t audio_fragments;
    uint64_t psi_fragments;
    uint64_t pid_fragment_count[TS_PID_COUNT];
    size_t next_slot;
} ContestContext;

void contest_init(ContestContext *ctx);
void contest_observe_packet(ContestContext *ctx, const TsPacket *pkt, const PsiContext *psi, uint64_t packet_index);
void contest_print_json(FILE *out, const ContestContext *contest, const Stats *stats,
                        const PsiContext *psi, uint64_t interval_index);

#endif
