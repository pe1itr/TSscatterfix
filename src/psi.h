#ifndef PSI_H
#define PSI_H

#include <stddef.h>
#include <stdint.h>

#include "ts_packet.h"

#define PSI_MAX_STREAMS 32
#define PSI_MAX_CANDIDATES 8

typedef struct {
    uint8_t data[TS_PACKET_SIZE];
    uint16_t pid;
    uint8_t version;
    uint32_t crc;
    double weight;
    uint64_t last_seen_packet;
    uint32_t seen_count;
    uint8_t valid;
} PsiCandidate;

typedef struct {
    uint8_t has_pat;
    uint8_t has_pmt;
    uint8_t has_sdt;
    uint16_t primary_pmt_pid;
    uint16_t service_id;
    uint16_t pcr_pid;
    uint16_t video_pids[PSI_MAX_STREAMS];
    uint8_t video_types[PSI_MAX_STREAMS];
    size_t video_count;
    uint16_t audio_pids[PSI_MAX_STREAMS];
    uint8_t audio_types[PSI_MAX_STREAMS];
    size_t audio_count;
    uint8_t cached_pat[TS_PACKET_SIZE];
    uint8_t cached_pmt[TS_PACKET_SIZE];
    uint8_t cached_sdt[TS_PACKET_SIZE];
    uint8_t has_cached_pat;
    uint8_t has_cached_pmt;
    uint8_t has_cached_sdt;
    char service_name[256];
    char provider_name[256];
    PsiCandidate pat_candidates[PSI_MAX_CANDIDATES];
    PsiCandidate pmt_candidates[PSI_MAX_CANDIDATES];
    PsiCandidate sdt_candidates[PSI_MAX_CANDIDATES];
    double best_pat_confidence;
    double best_pmt_confidence;
    double best_sdt_confidence;
} PsiContext;

void psi_init(PsiContext *ctx);
void psi_observe_packet(PsiContext *ctx, const TsPacket *pkt, uint64_t packet_index, int verbose);
uint8_t psi_pid_is_pmt(const PsiContext *ctx, uint16_t pid);
uint8_t psi_pid_is_known_si(const PsiContext *ctx, uint16_t pid);
uint8_t psi_packet_has_valid_section(const PsiContext *ctx, const TsPacket *pkt);
uint32_t psi_mpeg_crc32(const uint8_t *data, size_t len);

#endif
